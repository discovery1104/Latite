#include "pch.h"
#include "MusicHelperClient.h"

#include "client/Omoti.h"

namespace {
	constexpr wchar_t kPipeName[] = LR"(\\.\pipe\OmotiMusicHelperPipe_v1)";
	constexpr wchar_t kHelperExeName[] = L"OmotiMusicHelper.exe";

	std::wstring readPipeLine(HANDLE pipe) {
		std::string response;
		char buffer[512]{};
		DWORD bytesRead = 0;
		while (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
			response.append(buffer, buffer + bytesRead);
			if (response.find('\n') != std::string::npos) break;
		}

		if (auto newline = response.find('\n'); newline != std::string::npos) {
			response.resize(newline);
		}

		return util::StrToWStr(response);
	}

	bool writePipeLine(HANDLE pipe, std::string const& line) {
		DWORD bytesWritten = 0;
		return WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &bytesWritten, nullptr) && bytesWritten == line.size();
	}
}

MusicHelperClient& MusicHelperClient::get() {
	static MusicHelperClient instance;
	return instance;
}

MusicHelperStatus MusicHelperClient::getStatus(bool force) {
	std::scoped_lock lock(clientMutex);
	auto now = GetTickCount64();
	if (!force && cachedStatus.helperAvailable && (now - lastStatusTick) < 150) {
		return cachedStatus;
	}

	cachedStatus = requestStatus();
	lastStatusTick = now;
	return cachedStatus;
}

bool MusicHelperClient::play(std::filesystem::path const& trackPath, float volume) {
	std::scoped_lock lock(clientMutex);
	json request = {
		{"command", "play"},
		{"path", util::WStrToStr(trackPath.wstring())},
		{"volume", std::clamp(volume, 0.f, 1.f)}
	};

	auto response = sendRequest(request);
	if (!response.has_value()) return false;

	cachedStatus = {};
	updateStatusFromJson(cachedStatus, *response);
	lastStatusTick = GetTickCount64();
	return cachedStatus.ok;
}

bool MusicHelperClient::pause() {
	std::scoped_lock lock(clientMutex);
	auto response = sendRequest({ {"command", "pause"} });
	if (!response.has_value()) return false;
	cachedStatus = {};
	updateStatusFromJson(cachedStatus, *response);
	lastStatusTick = GetTickCount64();
	return cachedStatus.ok;
}

bool MusicHelperClient::resume() {
	std::scoped_lock lock(clientMutex);
	auto response = sendRequest({ {"command", "resume"} });
	if (!response.has_value()) return false;
	cachedStatus = {};
	updateStatusFromJson(cachedStatus, *response);
	lastStatusTick = GetTickCount64();
	return cachedStatus.ok;
}

bool MusicHelperClient::stop() {
	std::scoped_lock lock(clientMutex);
	auto response = sendRequest({ {"command", "stop"} }, false);
	if (!response.has_value()) return false;
	cachedStatus = {};
	updateStatusFromJson(cachedStatus, *response);
	lastStatusTick = GetTickCount64();
	return cachedStatus.ok;
}

bool MusicHelperClient::shutdown() {
	std::scoped_lock lock(clientMutex);
	auto response = sendRequest({ {"command", "shutdown"} }, false);
	cachedStatus = {};
	lastStatusTick = 0;
	if (!response.has_value()) return false;
	return response->value("ok", false);
}

bool MusicHelperClient::seek(int targetMs) {
	std::scoped_lock lock(clientMutex);
	auto response = sendRequest({ {"command", "seek"}, {"ms", std::max(0, targetMs)} }, false);
	if (!response.has_value()) return false;
	cachedStatus = {};
	updateStatusFromJson(cachedStatus, *response);
	lastStatusTick = GetTickCount64();
	return cachedStatus.ok;
}

bool MusicHelperClient::setVolume(float volume) {
	std::scoped_lock lock(clientMutex);
	auto response = sendRequest({ {"command", "volume"}, {"volume", std::clamp(volume, 0.f, 1.f)} }, false);
	if (!response.has_value()) return false;
	cachedStatus = {};
	updateStatusFromJson(cachedStatus, *response);
	lastStatusTick = GetTickCount64();
	return cachedStatus.ok;
}

std::wstring MusicHelperClient::getLastError() const {
	std::scoped_lock lock(clientMutex);
	return lastError;
}

MusicHelperStatus MusicHelperClient::requestStatus() {
	MusicHelperStatus status;
	auto response = sendRequest({ {"command", "status"} }, false);
	if (!response.has_value()) {
		status.error = lastError;
		return status;
	}

	updateStatusFromJson(status, *response);
	return status;
}

std::optional<json> MusicHelperClient::sendRequest(json const& request, bool allowLaunch) {
	json requestPayload = request;
	requestPayload["ownerPid"] = static_cast<int>(GetCurrentProcessId());

	auto openPipe = [&]() -> HANDLE {
		if (!WaitNamedPipeW(kPipeName, 150)) {
			return INVALID_HANDLE_VALUE;
		}

		return CreateFileW(
			kPipeName,
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			0,
			nullptr
		);
	};

	HANDLE pipe = openPipe();
	if (pipe == INVALID_HANDLE_VALUE && allowLaunch && ensureHelperStarted()) {
		pipe = openPipe();
	}
	if (pipe == INVALID_HANDLE_VALUE) {
		lastError = L"Music helper pipe is unavailable";
		return std::nullopt;
	}

	std::string payload = requestPayload.dump();
	payload.push_back('\n');

	if (!writePipeLine(pipe, payload)) {
		lastError = L"Could not write to music helper pipe";
		CloseHandle(pipe);
		return std::nullopt;
	}

	FlushFileBuffers(pipe);
	std::wstring responseLine = readPipeLine(pipe);
	CloseHandle(pipe);

	if (responseLine.empty()) {
		lastError = L"Music helper returned an empty response";
		return std::nullopt;
	}

	try {
		lastError.clear();
		return json::parse(util::WStrToStr(responseLine));
	}
	catch (...) {
		lastError = L"Music helper returned invalid JSON";
		return std::nullopt;
	}
}

bool MusicHelperClient::ensureHelperStarted() {
	auto helperPath = resolveHelperPath();
	if (helperPath.empty() || !std::filesystem::exists(helperPath)) {
		lastError = L"OmotiMusicHelper.exe was not found next to the DLL";
		return false;
	}

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};

	std::wstring commandLine = L"\"" + helperPath.wstring() + L"\"";
	if (!CreateProcessW(
		helperPath.c_str(),
		commandLine.data(),
		nullptr,
		nullptr,
		FALSE,
		CREATE_NO_WINDOW | DETACHED_PROCESS,
		nullptr,
		helperPath.parent_path().c_str(),
		&si,
		&pi
	)) {
		lastError = std::format(L"Could not start helper process ({})", GetLastError());
		return false;
	}

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	for (int i = 0; i < 40; i++) {
		Sleep(100);
		auto response = sendRequest({ {"command", "ping"} }, false);
		if (response.has_value()) return true;
	}

	if (lastError.empty()) lastError = L"Music helper did not start in time";
	return false;
}

void MusicHelperClient::updateStatusFromJson(MusicHelperStatus& status, json const& response) const {
	status.ok = response.value("ok", false);
	status.helperAvailable = true;
	status.state = util::StrToWStr(response.value("state", std::string("stopped")));
	status.path = util::StrToWStr(response.value("path", std::string("")));
	status.error = util::StrToWStr(response.value("error", std::string("")));
	status.positionMs = response.value("positionMs", 0);
	status.durationMs = response.value("durationMs", -1);
	status.volume = response.value("volume", 1.f);
}

std::filesystem::path MusicHelperClient::resolveHelperPath() const {
	wchar_t dllPath[MAX_PATH]{};
	auto len = GetModuleFileNameW(Omoti::get().dllInst, dllPath, MAX_PATH);
	if (len > 0 && len < MAX_PATH) {
		auto baseDir = std::filesystem::path(dllPath).parent_path();
		return baseDir / kHelperExeName;
	}

	return std::filesystem::path(L"E:\\latiteskid") / kHelperExeName;
}
