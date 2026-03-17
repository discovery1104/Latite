# Omoti Client
[![Discord](https://img.shields.io/discord/885656043521179680)](https://discord.gg/GpV3w5tyBs)
[![GitHub release](https://img.shields.io/github/v/release/Imrglop/Omoti-Releases)](https://github.com/Imrglop/Omoti-Releases/releases/latest)
[![Omoti Nightly](https://github.com/OmotiClient/Omoti/actions/workflows/nightly-build.yml/badge.svg)](https://github.com/OmotiClient/Omoti/actions/workflows/nightly-build.yml)

**Omoti Client** is a legitimate DLL modification for Minecraft Windows 10/11 Edition featuring a clean UI, 30+ customizable mods, and a powerful plugin system.

![Demo image](https://github.com/user-attachments/assets/0862d42f-ac15-4bd6-9395-536ce27d7ed4)

## Features
- **Clean Interface**: Intuitive UI with search and filtering
- **30+ Mods**: Motion Blur, Keystrokes, Zoom, FPS Counter, Toggle Sprint, and more
- **Plugin System**: Add community plugins or create your own using JavaScript/TypeScript
- **Customization**: Accent colors, module positions, keybinds, and font settings
- **Injector (Launcher)**: Easy injection with the dedicated Omoti Injector

## Building
1. Clone the repository.
2. [Install Visual Studio or Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/).
3. Install [MinGW-w64](https://www.mingw-w64.org/). The installation directory must be in your system's PATH. To test this, run the `ld` command in a terminal. If MinGW is successfully installed, it should output `ld: no input files`.
4. Use CMake to build the project.

## Contributing
We welcome people to contribute code via making a PR (Pull Request) to the Client or [Injector](https://github.com/Plextora/OmotiInjector). Just make sure to ping us in our [Discord Server](https://discord.gg/GpV3w5tyBs) if we don't get to reviewing your PR in a timely manner :)


## Installation
### Recommended Method
1. Download the [Omoti Installer](https://github.com/Imrglop/Omoti-Releases/raw/main/injector/Installer.exe)
2. Run the installer
3. Run the Injector from the desktop shortcut, Windows Start menu, or from `%programfiles%\Omoti Injector\Omoti Injector.exe`
4. Press "Launch" when in the Injector.


### Alternative Options
- [Omoti Minimal](https://Omoti-client.is-from.space/r/Omoti_Minimal.exe) (for antivirus issues/India users)
- [DLL Download](https://github.com/Imrglop/Omoti-Releases/releases/latest/download/Omoti.dll) (for using Omoti with other Injector's)
- **Jiayi Launcher Import**:  
  Use this command (Win + R) to import Omoti in Jiayi Launcher:
  ```bash
  jiayi://addmod/https://github.com/Imrglop/Omoti-Releases/releases/latest/download/Omoti.dll
  ```

[Watch Installation Tutorial](https://youtu.be/h3v849ayuZY)

## Plugins
Enhance your experience with community-made plugins:
```console
.plugin install [plugin-name]
.plugin load [plugin-name]
```
- [Available Plugins](https://Omoti.net/plugins/)
- [Plugin Documentation](https://Omotiscripting.github.io)
- [Plugin Repository](https://github.com/OmotiScripting/Scripts)

## FAQ
<details>

<details>
<summary>Why is it flagged as a virus?</summary>
This is a false positive due to DLL injection techniques. Omoti is completely safe. <a href="https://Omoti.net/#faq">Learn more</a>
</details>

<details>
<summary>Can I use this on mobile?</summary>
No  Echeck out our Android project <a href="https://atlasclient.net">Atlas Client</a> instead.
</details>
</details>

[View Full FAQ](https://Omoticlient.com/#faq)

## Community
- [Discord Server](https://discord.gg/GpV3w5tyBs)
- [Twitter](https://twitter.com/OmotiClient)
- [YouTube](https://youtube.com/@OmotiClientMC)

> **Note: These are the only official social medias Omoti Client has. If an entity is claiming to be Omoti Client and does not have the same socials as the ones listed above, they are impersonating us.**

## License
By using Omoti Client, you agree to our [License Terms](https://raw.githubusercontent.com/OmotiClient/Omoti/refs/heads/master/LICENSE).

---------------------------

**Disclaimer**: Omoti Client is not affiliated with Mojang or Microsoft in any way, shape, or form. Use at your own risk on multiplayer servers.
