; What the installer (SpokenPlayersVoicepack.iss) needs from its build: tools\Build-Installer.ps1 passes
; Version and Url (where the voice models are downloaded from, ending in /).

#define AppName "Spoken Players Voicepack"
#define Publisher "Spoken Players"
; Never change it: Windows knows the installed voicepack by it, and a newer setup finds it by it.
#define AppGuid "761B8789-B864-4C4E-98B1-5A0519A0DAA9"
#define AppId "{{" + AppGuid + "}"
#define SetupName "SpokenPlayersVoicepack-Setup"

#ifndef Version
  #error Build with tools\Build-Installer.ps1 (it passes Version and Url)
#endif
