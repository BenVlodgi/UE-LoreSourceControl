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
  
  <a href=""><img alt="UE 4.26" src="https://img.shields.io/badge/UE-4.26-green?logo=unrealengine"></a>
  <a href=""><img alt="UE 4.27" src="https://img.shields.io/badge/UE-4.27-green?logo=unrealengine"></a>
  
  <a href=""><img alt="UE 5.0" src="https://img.shields.io/badge/UE-5.0-green?logo=unrealengine"></a>
  <a href=""><img alt="UE 5.1" src="https://img.shields.io/badge/UE-5.1-green?logo=unrealengine"></a>
  <a href=""><img alt="UE 5.2" src="https://img.shields.io/badge/UE-5.2-green?logo=unrealengine"></a>
  <a href=""><img alt="UE 5.3" src="https://img.shields.io/badge/UE-5.3-green?logo=unrealengine"></a>
  <a href=""><img alt="UE 5.4" src="https://img.shields.io/badge/UE-5.4-green?logo=unrealengine"></a>
  <a href=""><img alt="UE 5.5" src="https://img.shields.io/badge/UE-5.5-green?logo=unrealengine"></a>
  <a href=""><img alt="UE 5.6" src="https://img.shields.io/badge/UE-5.6-green?logo=unrealengine"></a>
  <a href=""><img alt="UE 5.7" src="https://img.shields.io/badge/UE-5.7-green?logo=unrealengine"></a>
  <a href=""><img alt="UE 5.8" src="https://img.shields.io/badge/UE-5.8-green?logo=unrealengine"></a>
</p>

</div>

Unreal Engine Plugin adds support for [Lore](https://lore.org) Version Control System.


## Setup

### 1. Install Plugin

Download and extract `LoreSourceControl` into your project's `Plugins/` directory.

The Plugin will automatically be set as enabled.
<img height="100" alt="LorePlugin" src="https://github.com/user-attachments/assets/fb4a07be-58eb-4ae2-a7b6-eb25147df72a" />


### 2. Set Lore as Revision Control Provider

In the Unreal Engine Editor, open **Revision Control**, pick **Lore**.
<img width="626" height="240" alt="image" src="https://github.com/user-attachments/assets/0da9a020-7886-4cb8-b4df-110b45e22959" />
<img width="406" height="173" alt="image" src="https://github.com/user-attachments/assets/a63bc7cd-d987-4695-b54a-0cbbfd9877bd" />


#### 2.1 Ensure the Lore CLI is Available

If you don't already have the Lore CLI installed, you can download the latest binaries locally for this project by clicking **Download latest Lore CLI**, or install the CLI yourself:
- Available on GitHub [EpicGames/lore/releases](https://github.com/EpicGames/lore/releases).
- Windows: `irm https://raw.githubusercontent.com/EpicGames/lore/main/scripts/install.ps1 | iex`
- macOS, Linux: `curl -fsSL https://raw.githubusercontent.com/EpicGames/lore/main/scripts/install.sh | bash`
