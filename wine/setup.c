/*
 * SpokenPlayersVoicepack-Wine.exe: the voicepack set up in a Wine or Proton prefix, for World of
 * Warcraft run on Linux. It runs inside the prefix WoW runs in: wine/install.sh starts it there, as
 * does a game launcher's "run an EXE in this prefix".
 *
 *  1. The voicepack's own setup, the one Windows runs (beside this file, else downloaded), where the
 *     player chooses the game and the voices are downloaded.
 *  2. The engine's certificate trusted, which that setup does with certutil, a stub in Wine.
 *  3. SapiFix (sapifix.dll, carried in this file) installed and registered: WoW's speech needs what
 *     it adds to Wine's SAPI (sapifix.c). Its certificate (carried too) trusted first: it is
 *     signed as the engine is. Then WoW told to speak with Windows' voices, which under Wine it
 *     does not by itself, and kept from what crashes it (set_up_wow).
 *  4. A line said the way WoW says it (a bookmarked line, into a WAV, followed by its events), then
 *     played, so the player hears that it works.
 *
 * SpokenPlayersVoicepack-Wine.exe [/quiet] [/game=forever|retail|all] [/novoicepack] [/test] [/uninstall]
 * /quiet: no windows, the voicepack's setup silent too (the game given, else the one chosen last
 * time); what happens on stdout; exit code 0 when all of it worked.
 * Log: %LOCALAPPDATA%\SapiFix\setup.log (and voicepack-setup.log, the voicepack setup's own).
 */

#define COBJMACROS
#define CONST_VTABLE
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <wincrypt.h>
#include <wininet.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

DEFINE_GUID(FORMAT_WaveFormatEx, 0xc31adbae, 0x527f, 0x4ff5, 0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c);

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define TITLE       L"Spoken Players Voicepack for Linux"
#define SITE        L"https://spokenplayers.sahnet.ee"
#define SETUP_NAME  L"SpokenPlayersVoicepack-Setup.exe"
#define SETUP_URL   SITE L"/download/" SETUP_NAME
#define TOKENS      L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens"
#define SETTINGS    L"SOFTWARE\\Spoken Players"

static BOOL quiet;
static WCHAR log_dir[MAX_PATH], log_path[MAX_PATH];

/* ---- telling the player --------------------------------------------------------------------- */

static void log_init(void)
{
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", log_dir, MAX_PATH)) GetTempPathW(MAX_PATH, log_dir);
    wcscat(log_dir, L"\\SapiFix");
    CreateDirectoryW(log_dir, NULL);
    swprintf(log_path, MAX_PATH, L"%ls\\setup.log", log_dir);
}

static void write_log(const WCHAR *text)
{
    char line[2048];
    SYSTEMTIME now;
    HANDLE file;
    DWORD written;
    int len;

    GetLocalTime(&now);
    len = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth, now.wDay,
                   now.wHour, now.wMinute, now.wSecond);
    len += WideCharToMultiByte(CP_UTF8, 0, text, -1, line + len, sizeof(line) - len - 2, NULL, NULL) - 1;
    line[len++] = '\n';
    file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, len, &written, NULL);
    CloseHandle(file);
}

/* A line of what happens: on stdout and in the log. */
static void say(const WCHAR *format, ...)
{
    WCHAR text[1024];
    va_list args;

    va_start(args, format);
    vswprintf(text, ARRAY_SIZE(text), format, args);
    va_end(args);
    wprintf(L"%ls\n", text);
    fflush(stdout);
    write_log(text);
}

/* A message for the player: a window (its answer returned), or with /quiet a line, answered yes. */
static int ask(UINT flags, const WCHAR *format, ...)
{
    WCHAR text[2048];
    va_list args;
    int answer;

    va_start(args, format);
    vswprintf(text, ARRAY_SIZE(text), format, args);
    va_end(args);
    write_log(text);
    if (quiet)
    {
        wprintf(L"%ls\n", text);
        return (flags & 0xf) == MB_YESNO ? IDYES : (flags & 0xf) == MB_RETRYCANCEL ? IDCANCEL : IDOK;
    }
    answer = MessageBoxW(NULL, text, TITLE, flags | MB_SETFOREGROUND | MB_TOPMOST);
    return answer;
}

/* A path in the prefix as Linux sees it, for messages (Wine's kernel32 says), else as it is. */
static void linux_path(const WCHAR *path, WCHAR *out, size_t size)
{
    char *(CDECL *unix_name)(const WCHAR *) =
        (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    char *name = unix_name ? unix_name(path) : NULL;

    if (name && MultiByteToWideChar(CP_UTF8, 0, name, -1, out, (int)size)) ;
    else lstrcpynW(out, path, (int)size);
    if (name) HeapFree(GetProcessHeap(), 0, name);
}

static const char *wine_version(void)
{
    const char *(CDECL *get)(void) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    return get ? get() : NULL;
}

/* WoW running in this prefix (its engine, SapiFix, can't be replaced then): its program's name. */
static BOOL wow_running(WCHAR *name, size_t size)
{
    PROCESSENTRY32W entry = { sizeof(entry) };
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    BOOL found = FALSE;

    if (snapshot == INVALID_HANDLE_VALUE) return FALSE;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            size_t len = wcslen(entry.szExeFile);

            if (len > 7 && !_wcsnicmp(entry.szExeFile, L"wow", 3) && !_wcsicmp(entry.szExeFile + len - 4, L".exe"))
            {
                lstrcpynW(name, entry.szExeFile, (int)size);
                found = TRUE;
            }
        } while (!found && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

/* ---- 1. the voicepack's setup ----------------------------------------------------------------- */

/* The setup: beside this file when it is there, else the newest, downloaded. */
static BOOL find_setup(WCHAR *path)
{
    WCHAR *slash;
    HRESULT hr;

    GetModuleFileNameW(NULL, path, MAX_PATH);
    if ((slash = wcsrchr(path, '\\'))) wcscpy(slash + 1, SETUP_NAME);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
    {
        say(L"The voicepack's setup: %ls (beside this file)", path);
        return TRUE;
    }
    GetTempPathW(MAX_PATH, path);
    wcscat(path, SETUP_NAME);
    say(L"Downloading the voicepack's setup from %ls ...", SETUP_URL);
    DeleteUrlCacheEntryW(SETUP_URL);
    if (FAILED(hr = URLDownloadToFileW(NULL, SETUP_URL, path, 0, NULL)))
    {
        say(L"The download failed: %#lx", hr);
        return FALSE;
    }
    return TRUE;
}

static BOOL read_setting(const WCHAR *name, WCHAR *value, DWORD size)
{
    DWORD bytes = size * sizeof(WCHAR);

    return !RegGetValueW(HKEY_LOCAL_MACHINE, SETTINGS, name, RRF_RT_REG_SZ, NULL, value, &bytes);
}

/* Runs the setup and waits for it: its own wizard, or with /quiet silent for the game. A silent
 * setup takes the game chosen last time (HKLM\SOFTWARE\Spoken Players: Game) and installs its races,
 * so the game given is written there first; with none there it stops at its game page. */
static DWORD run_setup(const WCHAR *setup, const WCHAR *game)
{
    WCHAR command[4096], setup_log[MAX_PATH], previous[32];
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process;
    DWORD code = 1;

    if (!quiet)
        swprintf(command, ARRAY_SIZE(command), L"\"%ls\"", setup);
    else
    {
        if (!game && read_setting(L"Game", previous, ARRAY_SIZE(previous))) game = previous;
        if (!game || (_wcsicmp(game, L"forever") && _wcsicmp(game, L"retail") && _wcsicmp(game, L"all")))
        {
            say(L"Which game's voices? Say /game=forever, /game=retail or /game=all.");
            return 1;
        }
        if (game != previous)
        {
            HKEY key;

            if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, SETTINGS, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &key, NULL))
            {
                RegSetValueExW(key, L"Game", 0, REG_SZ, (const BYTE *)game, (wcslen(game) + 1) * sizeof(WCHAR));
                RegCloseKey(key);
            }
        }
        swprintf(setup_log, MAX_PATH, L"%ls\\voicepack-setup.log", log_dir);
        swprintf(command, ARRAY_SIZE(command), L"\"%ls\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART \"/LOG=%ls\"", setup, setup_log);
        say(L"Installing the voices for %ls silently (the first time downloads them: some minutes) ...",
            !_wcsicmp(game, L"forever") ? L"WoW Forever" : !_wcsicmp(game, L"retail") ? L"retail WoW" : L"both games");
    }

    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
    {
        say(L"Could not start the setup: error %lu", GetLastError());
        return 1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    say(L"The voicepack's setup ended with code %lu%ls", code, code ? L"" : L" (installed)");
    return code;
}

/* Where the voicepack is installed (its voices' DataDir says), and the voice to try: the Orc's
 * if it has it, else the first. FALSE when no Spoken Players voice is installed. */
static BOOL find_voicepack(WCHAR *dir, size_t dir_size, WCHAR *voice, size_t voice_size)
{
    WCHAR name[256], data[MAX_PATH];
    HKEY tokens;
    DWORD i, len, bytes;
    BOOL found = FALSE;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TOKENS, 0, KEY_READ | KEY_WOW64_64KEY, &tokens)) return FALSE;
    for (i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW(tokens, i, name, &len, NULL, NULL, NULL, NULL); i++)
    {
        WCHAR *tail;

        if (_wcsnicmp(name, L"SpokenPlayers_", 14)) continue;
        bytes = sizeof(data);
        if (RegGetValueW(tokens, name, L"DataDir", RRF_RT_REG_SZ, NULL, data, &bytes)) continue;
        if ((tail = wcsrchr(data, '\\')) && !_wcsicmp(tail, L"\\espeak-ng-data")) *tail = 0;
        if (!found || !_wcsicmp(name, L"SpokenPlayers_orc_male"))
        {
            lstrcpynW(dir, data, (int)dir_size);
            lstrcpynW(voice, name, (int)voice_size);
        }
        found = TRUE;
    }
    RegCloseKey(tokens);
    return found;
}

/* ---- 2. the certificates -------------------------------------------------------------------- */

static const WCHAR *trust_stores[] = { L"Root", L"TrustedPublisher" };

/* Trusts a certificate as certutil -addstore does (Root and TrustedPublisher of the machine): a
 * store keeps a certificate as a "Blob" of properties, its SHA-1 (property 3) and itself (property
 * 32), each {id, 1, size} and its bytes. `thumbprint` gets its SHA-1 in hex. */
static BOOL trust_der(const BYTE *der, DWORD size, WCHAR *thumbprint)
{
    WCHAR key_path[256];
    BYTE sha1[20], *blob;
    DWORD hash_size = sizeof(sha1), blob_size = 12 + sizeof(sha1) + 12 + size, i, header[3];
    BOOL ok = TRUE;

    if (!CryptHashCertificate(0, CALG_SHA1, 0, der, size, sha1, &hash_size) || !(blob = malloc(blob_size)))
        return FALSE;
    header[0] = 3; header[1] = 1; header[2] = sizeof(sha1);
    memcpy(blob, header, 12);
    memcpy(blob + 12, sha1, sizeof(sha1));
    header[0] = 0x20; header[2] = size;
    memcpy(blob + 12 + sizeof(sha1), header, 12);
    memcpy(blob + 24 + sizeof(sha1), der, size);
    for (i = 0; i < sizeof(sha1); i++) swprintf(thumbprint + i * 2, 3, L"%02X", sha1[i]);

    for (i = 0; i < ARRAY_SIZE(trust_stores); i++)
    {
        HKEY key;

        swprintf(key_path, ARRAY_SIZE(key_path), L"Software\\Microsoft\\SystemCertificates\\%ls\\Certificates\\%ls",
                 trust_stores[i], thumbprint);
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &key, NULL))
        {
            ok = FALSE;
            continue;
        }
        if (RegSetValueExW(key, L"Blob", 0, REG_BINARY, blob, blob_size)) ok = FALSE;
        RegCloseKey(key);
    }
    free(blob);
    return ok;
}

static void untrust(const WCHAR *thumbprint)
{
    WCHAR key_path[256];
    DWORD i;

    for (i = 0; i < ARRAY_SIZE(trust_stores); i++)
    {
        swprintf(key_path, ARRAY_SIZE(key_path), L"Software\\Microsoft\\SystemCertificates\\%ls\\Certificates\\%ls",
                 trust_stores[i], thumbprint);
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key_path, KEY_WOW64_64KEY, 0);
    }
}

/* The engine's, beside it, which the voicepack's setup trusts with certutil, a stub in Wine. */
static BOOL trust_certificate(const WCHAR *dir)
{
    WCHAR path[MAX_PATH], thumbprint[41];
    BYTE *der = NULL;
    DWORD size, read;
    HANDLE file;
    BOOL ok = FALSE;

    swprintf(path, MAX_PATH, L"%ls\\spoken_voice_engine.cer", dir);
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        say(L"No engine certificate at %ls", path);
        return FALSE;
    }
    size = GetFileSize(file, NULL);
    if (size && size < 65536 && (der = malloc(size)) && ReadFile(file, der, size, &read, NULL) && read == size)
        ok = trust_der(der, size, thumbprint);
    if (ok) say(L"The engine's certificate is trusted (%ls).", thumbprint);
    else say(L"Could not trust the engine's certificate (%ls).", path);
    CloseHandle(file);
    free(der);
    return ok;
}

/* ---- 3. SapiFix --------------------------------------------------------------------------- */

#define SAPIFIX_KEY L"Software\\SapiFix"

/* Its certificate, carried in this file: SapiFix is signed as the engine is (WoW loads no SAPI
 * engine whose signature it does not trust: DOCS.md, Signing), by build.sh, with a certificate made
 * for each build. The one trusted is noted under SAPIFIX_KEY, so the next build's replaces it and
 * uninstalling takes it out. */
static BOOL trust_sapifix(void)
{
    HRSRC resource = FindResourceW(NULL, L"SAPIFIXCER", (LPCWSTR)RT_RCDATA);
    WCHAR thumbprint[41], before[41];
    DWORD bytes = sizeof(before);
    const BYTE *der;
    HKEY key;

    if (!resource || !(der = LockResource(LoadResource(NULL, resource))))
    {
        say(L"This file carries no certificate for SapiFix: build it with wine/build.sh.");
        return FALSE;
    }
    if (!trust_der(der, SizeofResource(NULL, resource), thumbprint))
    {
        say(L"Could not trust SapiFix's certificate.");
        return FALSE;
    }
    if (!RegGetValueW(HKEY_LOCAL_MACHINE, SAPIFIX_KEY, L"Certificate", RRF_RT_REG_SZ, NULL, before, &bytes) &&
        wcscmp(before, thumbprint))
        untrust(before);
    if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, SAPIFIX_KEY, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &key, NULL))
    {
        RegSetValueExW(key, L"Certificate", 0, REG_SZ, (const BYTE *)thumbprint, (wcslen(thumbprint) + 1) * sizeof(WCHAR));
        RegCloseKey(key);
    }
    say(L"SapiFix's certificate is trusted (%ls).", thumbprint);
    return TRUE;
}

static void untrust_sapifix(void)
{
    WCHAR thumbprint[41];
    DWORD bytes = sizeof(thumbprint);

    if (!RegGetValueW(HKEY_LOCAL_MACHINE, SAPIFIX_KEY, L"Certificate", RRF_RT_REG_SZ, NULL, thumbprint, &bytes))
        untrust(thumbprint);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, SAPIFIX_KEY, KEY_WOW64_64KEY, 0);
}

static void sapifix_path(WCHAR *dir, WCHAR *dll)
{
    if (!GetEnvironmentVariableW(L"ProgramW6432", dir, MAX_PATH) && !GetEnvironmentVariableW(L"ProgramFiles", dir, MAX_PATH))
        wcscpy(dir, L"C:\\Program Files");
    wcscat(dir, L"\\SapiFix");
    swprintf(dll, MAX_PATH, L"%ls\\sapifix.dll", dir);
}

/* Whether `path` holds exactly `size` bytes of `data`. */
static BOOL same_file(const WCHAR *path, const void *data, DWORD size)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    BYTE *have;
    DWORD read = 0;
    BOOL same = FALSE;

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (GetFileSize(file, NULL) == size && (have = malloc(size)))
    {
        same = ReadFile(file, have, size, &read, NULL) && read == size && !memcmp(have, data, size);
        free(have);
    }
    CloseHandle(file);
    return same;
}

static BOOL install_sapifix(void)
{
    WCHAR dir[MAX_PATH], dll[MAX_PATH], fresh[MAX_PATH];
    HRSRC resource = FindResourceW(NULL, L"SAPIFIX", (LPCWSTR)RT_RCDATA);
    HRESULT (WINAPI *register_server)(void);
    const void *data;
    DWORD size, written;
    HMODULE module;
    HANDLE file;
    HRESULT hr;

    if (!resource || !(data = LockResource(LoadResource(NULL, resource))))
    {
        say(L"This file carries no sapifix.dll: build it with wine/build.sh.");
        return FALSE;
    }
    size = SizeofResource(NULL, resource);
    if (!trust_sapifix()) return FALSE;
    sapifix_path(dir, dll);
    CreateDirectoryW(dir, NULL);

    if (same_file(dll, data, size))
        say(L"SapiFix is up to date: %ls", dll);
    else
    {
        swprintf(fresh, MAX_PATH, L"%ls.new", dll);
        file = CreateFileW(fresh, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE || !WriteFile(file, data, size, &written, NULL) || written != size)
        {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            say(L"Could not write %ls: error %lu", fresh, GetLastError());
            return FALSE;
        }
        CloseHandle(file);
        if (!MoveFileExW(fresh, dll, MOVEFILE_REPLACE_EXISTING))
        {
            say(L"Could not replace %ls (error %lu): is World of Warcraft running?", dll, GetLastError());
            DeleteFileW(fresh);
            return FALSE;
        }
        say(L"SapiFix installed: %ls", dll);
    }

    if (!(module = LoadLibraryW(dll)) || !(register_server = (void *)GetProcAddress(module, "DllRegisterServer")))
    {
        say(L"Could not load %ls: error %lu", dll, GetLastError());
        return FALSE;
    }
    if (FAILED(hr = register_server()))
    {
        say(L"Could not register SapiFix: %#lx", hr);
        return FALSE;
    }
    say(L"SapiFix registered: WoW's speech goes through it.");
    return TRUE;
}

static BOOL uninstall_sapifix(void)
{
    WCHAR dir[MAX_PATH], dll[MAX_PATH];
    HRESULT (WINAPI *unregister_server)(void);
    HMODULE module;

    sapifix_path(dir, dll);
    if ((module = LoadLibraryW(dll)) && (unregister_server = (void *)GetProcAddress(module, "DllUnregisterServer")))
        unregister_server();
    if (module) FreeLibrary(module);
    DeleteFileW(dll);
    RemoveDirectoryW(dir);
    untrust_sapifix();
    say(L"SapiFix is out: %ls", dir);
    return TRUE;
}

/* What WoW needs, in each of its game folders whose game is Wow.exe, WowT.exe or WowB.exe (retail,
 * its test realms and betas, WoW Forever; not WowClassic*.exe):
 *  - winePlatformTTS in WTF\Config.wtf: under Wine, WoW speaks with two voices of its own (Vivox's
 *    "en_US male" and "en_US female") instead of Windows', unless it is set ("Use Windows TTS on
 *    Wine. Disabled by default as it can cause crashes": Wine's SAPI lacks what WoW uses).
 *  - dxcore.dll off for the game (Wine's AppDefaults): the engine's ONNX Runtime looks for GPUs
 *    through DXCore as it loads a model, and Wine's DXCore is built on wined3d, which opens an
 *    OpenGL context; in WoW's process that fails, and Wine aborts the game. The engine speaks on
 *    the CPU: without DXCore it loads quicker. */
static const WCHAR *wow_games[] = { L"Wow.exe", L"WowT.exe", L"WowB.exe" };

static void set_platform_tts(const WCHAR *folder, BOOL on)
{
    static const char line[] = "SET winePlatformTTS \"1\"\r\n";
    WCHAR path[MAX_PATH], fresh[MAX_PATH];
    char *text = NULL, *out, *p, *next;
    DWORD size = 0, read = 0, written;
    HANDLE file;

    swprintf(path, MAX_PATH, L"%ls\\WTF", folder);
    if (on) CreateDirectoryW(path, NULL);
    wcscat(path, L"\\Config.wtf");
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        size = GetFileSize(file, NULL);
        if (size < (1 << 20) && (text = malloc(size + 1)) && ReadFile(file, text, size, &read, NULL)) text[read] = 0;
        CloseHandle(file);
        if (!text || read != size) { free(text); return; }
    }
    else if (!on) return;
    if (!(out = malloc(size + 2 + sizeof(line)))) { free(text); return; }

    /* Every line but this setting's, then it, on. */
    out[0] = 0;
    for (p = text; p && *p; p = next)
    {
        size_t len = (next = strchr(p, '\n')) ? (size_t)(++next - p) : strlen(p);
        if (!next) next = p + len;
        if (len < 20 || _strnicmp(p, "SET winePlatformTTS ", 20)) strncat(out, p, len);
    }
    if (on)
    {
        if (out[0] && out[strlen(out) - 1] != '\n') strcat(out, "\r\n");
        strcat(out, line);
    }

    swprintf(fresh, MAX_PATH, L"%ls.new", path);
    file = CreateFileW(fresh, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE && WriteFile(file, out, strlen(out), &written, NULL) && written == strlen(out))
    {
        CloseHandle(file);
        if (MoveFileExW(fresh, path, MOVEFILE_REPLACE_EXISTING))
            say(on ? L"WoW speaks with Windows' voices: %ls" : L"WoW speaks with its own voices again: %ls", path);
    }
    else if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    DeleteFileW(fresh);
    free(text);
    free(out);
}

static void set_dxcore_off(const WCHAR *game, BOOL off)
{
    WCHAR key_path[128];
    HKEY key;

    swprintf(key_path, ARRAY_SIZE(key_path), L"Software\\Wine\\AppDefaults\\%ls\\DllOverrides", game);
    if (!off)
    {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, key_path, L"dxcore");
        return;
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key_path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL)) return;
    RegSetValueExW(key, L"dxcore", 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR));
    RegCloseKey(key);
}

/* The folders WoW's game folders are in: where Battle.net says it put them, and the usual one. */
static int wow_roots(WCHAR roots[][MAX_PATH], int max)
{
    WCHAR name[128], path[MAX_PATH], *tail;
    HKEY key;
    DWORD i, len, bytes;
    int count = 0, j;

    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", path, MAX_PATH - 20))
    {
        wcscat(path, L"\\World of Warcraft");
        wcscpy(roots[count++], path);
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Blizzard Entertainment\\World of Warcraft", 0,
                      KEY_READ | KEY_WOW64_32KEY, &key))
        return count;
    for (i = 0; count < max; i++)
    {
        /* the key's own InstallPath first, then its subkeys' (Beta, PTR, ...) */
        if (i) { len = ARRAY_SIZE(name); if (RegEnumKeyExW(key, i - 1, name, &len, NULL, NULL, NULL, NULL)) break; }
        bytes = sizeof(path);
        if (RegGetValueW(key, i ? name : NULL, L"InstallPath", RRF_RT_REG_SZ, NULL, path, &bytes)) continue;
        while ((tail = wcsrchr(path, '\\')) && !tail[1]) *tail = 0;  /* the game folder: its parent */
        if (!(tail = wcsrchr(path, '\\'))) continue;
        *tail = 0;
        for (j = 0; j < count && _wcsicmp(roots[j], path); j++) ;
        if (j == count) wcscpy(roots[count++], path);
    }
    RegCloseKey(key);
    return count;
}

/* Sets WoW up (`on`) or back as it was, in every game folder found; how many. */
static int set_up_wow(BOOL on)
{
    WCHAR roots[8][MAX_PATH], pattern[MAX_PATH], folder[MAX_PATH], exe[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    int count = wow_roots(roots, ARRAY_SIZE(roots)), done = 0, r;
    DWORD g;

    for (r = 0; r < count; r++)
    {
        swprintf(pattern, MAX_PATH, L"%ls\\_*_", roots[r]);
        if ((find = FindFirstFileW(pattern, &found)) == INVALID_HANDLE_VALUE) continue;
        do
        {
            BOOL game = FALSE;

            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            swprintf(folder, MAX_PATH, L"%ls\\%ls", roots[r], found.cFileName);
            for (g = 0; g < ARRAY_SIZE(wow_games); g++)
            {
                swprintf(exe, MAX_PATH, L"%ls\\%ls", folder, wow_games[g]);
                if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) continue;
                set_dxcore_off(wow_games[g], on);
                game = TRUE;
            }
            if (!game) continue;
            set_platform_tts(folder, on);
            done++;
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    return done;
}

/* ---- 4. a line said as WoW says it ------------------------------------------------------------ */

/* Speaks with `voice` (a token's key name) as WoW does: the voice found in the voices' category,
 * a WAV bound with SpStream::BindToFile, the line between two bookmarks, its events followed through
 * a Win32 event until the stream ends. `wav` is where it was written. */
static HRESULT say_test_line(const WCHAR *voice, WCHAR *wav, WCHAR *problem, size_t problem_size)
{
    ULONGLONG interest = SPFEI(SPEI_START_INPUT_STREAM) | SPFEI(SPEI_END_INPUT_STREAM) | SPFEI(SPEI_TTS_BOOKMARK);
    WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 1, 44100, 88200, 2, 16, 0 };
    const WCHAR *line = wcsstr(voice, L"orc") ? L"Lok'tar ogar! The Spoken Players voices are ready."
                                              : L"Well met! The Spoken Players voices are ready.";
    ISpObjectTokenCategory *category = NULL;
    IEnumSpObjectTokens *tokens = NULL;
    ISpObjectToken *token = NULL, *chosen = NULL;
    ISpStream *stream = NULL;
    ISpVoice *speaker = NULL;
    WCHAR xml[512];
    ULONG count = 0, i, number = 0, bookmarks = 0;
    BOOL started = FALSE, ended = FALSE;
    STATSTG stat = {0};
    DWORD start;
    HRESULT hr;

    GetTempPathW(MAX_PATH, wav);
    wcscat(wav, L"SpokenPlayers-test.wav");
    swprintf(xml, ARRAY_SIZE(xml), L"<bookmark mark='Start'/>%ls<bookmark mark='End'/>", line);
    problem[0] = 0;

#define STEP(what, call) if (FAILED(hr = (call))) { swprintf(problem, problem_size, L"%ls failed: %#lx", what, hr); goto done; }
    STEP(L"The voices' category", CoCreateInstance(&CLSID_SpObjectTokenCategory, NULL, CLSCTX_ALL,
                                                   &IID_ISpObjectTokenCategory, (void **)&category));
    STEP(L"Listing the voices", ISpObjectTokenCategory_SetId(category, SPCAT_VOICES, FALSE));
    STEP(L"Listing the voices", ISpObjectTokenCategory_EnumTokens(category, NULL, NULL, &tokens));
    IEnumSpObjectTokens_GetCount(tokens, &count);
    for (i = 0; i < count && !chosen; i++)
    {
        WCHAR *id = NULL;

        if (FAILED(IEnumSpObjectTokens_Item(tokens, i, &token))) continue;
        ISpObjectToken_GetId(token, &id);
        if (id && wcslen(id) >= wcslen(voice) && !_wcsicmp(id + wcslen(id) - wcslen(voice), voice))
        {
            chosen = token;
            ISpObjectToken_AddRef(chosen);
        }
        CoTaskMemFree(id);
        ISpObjectToken_Release(token);
    }
    if (!chosen)
    {
        swprintf(problem, problem_size, L"the voice %ls is not among the %lu voices", voice, count);
        hr = E_FAIL;
        goto done;
    }
    STEP(L"Making a voice", CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL, &IID_ISpVoice, (void **)&speaker));
    STEP(L"Making a stream", CoCreateInstance(&CLSID_SpStream, NULL, CLSCTX_ALL, &IID_ISpStream, (void **)&stream));
    STEP(L"BindToFile", ISpStream_BindToFile(stream, wav, SPFM_CREATE_ALWAYS, &FORMAT_WaveFormatEx, &wfx, 0));
    STEP(L"SetVoice", ISpVoice_SetVoice(speaker, chosen));
    STEP(L"SetOutput", ISpVoice_SetOutput(speaker, (IUnknown *)stream, TRUE));
    STEP(L"SetInterest", ISpVoice_SetInterest(speaker, interest, interest));
    STEP(L"SetNotifyWin32Event", ISpVoice_SetNotifyWin32Event(speaker));
    start = GetTickCount();
    STEP(L"Speak", ISpVoice_Speak(speaker, xml, SPF_ASYNC, &number));

    /* The first line of a voice loads its model: some seconds, more on a slow PC. */
    while (!ended)
    {
        SPEVENT event;
        ULONG got;

        if (ISpVoice_WaitForNotifyEvent(speaker, 60000) != S_OK)
        {
            swprintf(problem, problem_size, L"the line did not end within a minute");
            hr = E_FAIL;
            goto done;
        }
        while (!ended && ISpVoice_GetEvents(speaker, 1, &event, &got) == S_OK && got)
        {
            if (event.eEventId == SPEI_START_INPUT_STREAM) started = TRUE;
            else if (event.eEventId == SPEI_TTS_BOOKMARK) bookmarks++;
            else if (event.eEventId == SPEI_END_INPUT_STREAM) ended = TRUE;
            if (event.elParamType == SPET_LPARAM_IS_STRING) CoTaskMemFree((void *)event.lParam);
        }
    }
    ISpStream_Stat(stream, &stat, STATFLAG_NONAME);
    STEP(L"Closing the WAV", ISpStream_Close(stream));
    say(L"Test line: %llu bytes of speech in %lu ms (events: start %ls, %lu bookmarks, end).",
        (unsigned long long)stat.cbSize.QuadPart, GetTickCount() - start, started ? L"yes" : L"NO", bookmarks);
    if (!stat.cbSize.QuadPart)
    {
        swprintf(problem, problem_size, L"the voice wrote no sound");
        hr = E_FAIL;
    }
    else if (!started || bookmarks != 2)
    {
        swprintf(problem, problem_size, L"its events were not all there");
        hr = E_FAIL;
    }
#undef STEP

done:
    if (stream) ISpStream_Release(stream);
    if (speaker) ISpVoice_Release(speaker);
    if (chosen) ISpObjectToken_Release(chosen);
    if (tokens) IEnumSpObjectTokens_Release(tokens);
    if (category) ISpObjectTokenCategory_Release(category);
    if (problem[0]) say(L"Test line: %ls", problem);
    return hr;
}

/* The test line said and, with windows, played; then the end of it all told. */
static int test(BOOL play)
{
    WCHAR dir[MAX_PATH], voice[256], wav[MAX_PATH], problem[512], logs[MAX_PATH];
    const char *version = wine_version();

    linux_path(log_dir, logs, ARRAY_SIZE(logs));
    if (!find_voicepack(dir, ARRAY_SIZE(dir), voice, ARRAY_SIZE(voice)))
    {
        ask(MB_ICONERROR | MB_OK, L"No Spoken Players voices are installed in this Wine prefix.\n\n"
            L"Run this again and let the voicepack's setup install them.");
        return 1;
    }
    if (FAILED(say_test_line(voice, wav, problem, ARRAY_SIZE(problem))))
    {
        ask(MB_ICONERROR | MB_OK, L"The voices are installed, but the test line could not be said: %ls.\n\n"
            L"This is Wine %hs; Proton 10 or Wine 10 or newer is needed.\n\nThe logs are in %ls",
            problem, version ? version : "(unknown)", logs);
        return 1;
    }
    if (play) PlaySoundW(wav, NULL, SND_FILENAME | SND_SYNC | SND_NODEFAULT);
    ask(MB_ICONINFORMATION | MB_OK, L"Done%ls\n\nStart World of Warcraft (restart it if it was open), then type /sp "
        L"in game to choose what is read aloud.\n\nWhen the voicepack has new voices, run this again.",
        play ? L": you should have heard a voice just now." : L".");
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    const WCHAR *game = NULL;
    BOOL voicepack = TRUE, only_test = FALSE, uninstall = FALSE;
    WCHAR setup[MAX_PATH], dir[MAX_PATH], voice[256], wow[MAX_PATH];
    const char *version = wine_version();
    DWORD code;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"/quiet")) quiet = TRUE;
        else if (!_wcsnicmp(argv[i], L"/game=", 6)) game = argv[i] + 6;
        else if (!_wcsicmp(argv[i], L"/novoicepack")) voicepack = FALSE;
        else if (!_wcsicmp(argv[i], L"/test")) only_test = TRUE;
        else if (!_wcsicmp(argv[i], L"/uninstall")) uninstall = TRUE;
        else
        {
            wprintf(L"usage: %ls [/quiet] [/game=forever|retail|all] [/novoicepack] [/test] [/uninstall]\n", argv[0]);
            return 2;
        }
    }

    if (!version)
    {
        ask(MB_ICONINFORMATION | MB_OK, L"This is for World of Warcraft on Linux, run through Wine or Proton.\n\n"
            L"On Windows, the voicepack's own setup is all you need: " SITE);
        return 2;
    }
    log_init();
    say(L"Spoken Players Voicepack for Linux, in Wine %hs", version);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (uninstall)
    {
        if (ask(MB_ICONQUESTION | MB_YESNO, L"Take SapiFix out of this Wine prefix? WoW speaks with its own two voices "
                L"again.\n\nThe voicepack stays: its own uninstaller is in its folder (unins000.exe).") != IDYES)
            return 2;
        set_up_wow(FALSE);
        return uninstall_sapifix() ? 0 : 1;
    }
    if (only_test) return test(!quiet);

    while (wow_running(wow, ARRAY_SIZE(wow)))
    {
        if (ask(MB_ICONWARNING | MB_RETRYCANCEL, L"World of Warcraft is running (%ls). Close it, then choose Retry.", wow) != IDRETRY)
            return 2;
    }

    if (!quiet && ask(MB_ICONINFORMATION | MB_OKCANCEL,
            L"This gets the Spoken Players voices talking in World of Warcraft on Linux.\n\n"
            L"1. The voicepack's own setup opens%ls: choose the WoW you play there, and it downloads the voices.\n"
            L"2. Then SapiFix is added, which WoW's speech needs under Wine and Proton.\n"
            L"3. Last, you hear a test voice.",
            voicepack ? L"" : L" (not this time: /novoicepack)") != IDOK)
        return 2;

    if (voicepack)
    {
        if (!find_setup(setup))
        {
            ask(MB_ICONERROR | MB_OK, L"Could not download the voicepack's setup.\n\nDownload %ls yourself from "
                SITE L", put it beside this file, and run this again.", SETUP_NAME);
            return 1;
        }
        if ((code = run_setup(setup, game)))
        {
            if (!find_voicepack(dir, ARRAY_SIZE(dir), voice, ARRAY_SIZE(voice)))
            {
                ask(MB_ICONERROR | MB_OK, L"The voicepack's setup did not finish (code %lu), and no voices are installed.", code);
                return 1;
            }
            if (ask(MB_ICONQUESTION | MB_YESNO, L"The voicepack's setup did not finish (code %lu). Go on with the voices "
                    L"installed before?", code) != IDYES)
                return 1;
        }
    }

    if (!find_voicepack(dir, ARRAY_SIZE(dir), voice, ARRAY_SIZE(voice)))
        return test(FALSE);
    trust_certificate(dir);
    if (!install_sapifix())
    {
        ask(MB_ICONERROR | MB_OK, L"SapiFix could not be installed: close World of Warcraft and run this again.");
        return 1;
    }
    if (!set_up_wow(TRUE))
        say(L"Found no World of Warcraft folder to set up. In WoW, type /console winePlatformTTS 1 once, then restart it.");
    return test(!quiet);
}
