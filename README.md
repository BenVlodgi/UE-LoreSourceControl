<!-- markdownlint-disable MD033 MD041 -->
<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://epicgames.github.io/lore/assets/icons/Lore_White_V1.svg">
  <img alt="Lore" src="https://epicgames.github.io/lore/assets/icons/Lore_Black_V1.svg" width="220">
</picture>

<h1>Lore Source Control for Unreal Engine</h1>

<p><strong>Community-made plugin. Unaffiliated with Epic Games. Lore is a trademark of Epic Games, Inc.</strong></p>

<p>
  <a href="https://github.com/EpicGames/lore/releases">Get Lore</a>
  &nbsp;&middot;&nbsp;
  <a href="https://epicgames.github.io/lore/">Lore docs</a>
  &nbsp;&middot;&nbsp;
  <a href="https://discord.gg/E4SFJKRPbg">Lore Discord</a>
</p>

<p>
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/License-MIT-informational"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-4.27-green?logo=unrealengine"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-5.0-green?logo=unrealengine"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-5.1-green?logo=unrealengine"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-5.2-green?logo=unrealengine"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-5.3-green?logo=unrealengine"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-5.4-green?logo=unrealengine"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-5.5-green?logo=unrealengine"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-5.6-green?logo=unrealengine"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-5.7-green?logo=unrealengine"></a>
  <a href=""><img alt="License: MIT" src="https://img.shields.io/badge/UE-5.8-green?logo=unrealengine"></a>
</p>

</div>

Unreal Engine Plugin adds support for [Lore](https://lore.org) Version Control System.

> [!NOTE]
> Lore is pre-1.0 and under active development. Interfaces, on-disk formats, and APIs may change between releases.


## Setup

### 1. Server

A Lore Server (local or remote) is required to host your repositories. 

If you don't already have a Lore Server: install one on the machine you want to host your repositories. Run the install script, then run `loreserver` (listens on `41337`):

- Windows (powershell): `irm https://raw.githubusercontent.com/EpicGames/lore/main/scripts/install.ps1 | iex`
- macOS, Linux: `curl -fsSL https://raw.githubusercontent.com/EpicGames/lore/main/scripts/install.sh | bash`

Or run it in Docker, building the image from a clone of the [Lore repository](https://github.com/EpicGames/lore):

```
docker build --platform linux/amd64 -f lore-server/Dockerfile -t lore-server .
docker run -d --name lore-server -p 41337:41337/tcp -p 41337:41337/udp -p 41339:41339 lore-server
```

### 2. Install Plugin

Download and extract `LoreSourceControl` into your project's `Plugins/` directory.

### 3. Set Lore as Revision Control Provider

In the Unreal Engine Editor, open **Revision Control**, pick **Lore**.
<img width="626" height="240" alt="image" src="https://github.com/user-attachments/assets/0da9a020-7886-4cb8-b4df-110b45e22959" />
<img width="406" height="173" alt="image" src="https://github.com/user-attachments/assets/a63bc7cd-d987-4695-b54a-0cbbfd9877bd" />


#### 3.1 Ensure the Lore CLI is Available

If you don't already have the Lore CLI installed, you can download the latest binaries locally for this project by clicking **Download latest Lore CLI**, or install the CLI yourself:
- Available on GitHub [EpicGames/lore/releases](https://github.com/EpicGames/lore/releases).
- Windows: `irm https://raw.githubusercontent.com/EpicGames/lore/main/scripts/install.ps1 | iex`
- macOS, Linux: `curl -fsSL https://raw.githubusercontent.com/EpicGames/lore/main/scripts/install.sh | bash`

#### 3.2 Connect to Server and Repository

**Revision Control > Connect to Revision Control > Lore**.

An existing Lore project connects directly; server, repository, branch, and identity are detected. To create a new repository:

```
lore --identity you@example.com repository create lore://your-server:41337/YourProject
lore stage --scan .
lore commit "Import"
lore push
```

Ignore generated folders in `.loreignore`: `Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/`.
