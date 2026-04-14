# KTPAmxxCurl - Claude Code Context

## Build Command
To build this project, use:
```bash
wsl bash -c "cd '/mnt/n/Nein_/KTP Git Projects/KTPAmxxCurl' && bash build_linux.sh"
```

This will:
1. Build using CMake
2. Output to `build/amxxcurl_ktp_i386.so`
3. Auto-stage to `N:\Nein_\KTP Git Projects\KTP DoD Server\serverfiles\dod\addons\ktpamx\modules\`

## Project Structure
- `build_linux.sh` - WSL build script
- `CMakeLists.txt` - CMake build configuration
- `src/` - C++ source files
- `bin/ReleaseDLL/` - Build output
- `amx_includes/` - AMX include files for plugins using this module

## Purpose
Non-blocking HTTP module for AMXX plugins. Enables async HTTP requests (GET/POST) with callbacks, used for Discord webhooks and HLStatsX integration.

## Build Output
| File | Destination |
|------|-------------|
| `amxxcurl_ktp_i386.so` | `addons/ktpamx/modules/` |

## Dependencies
- CMake
- libcurl (linked statically from deps/)
- GCC with 32-bit support

## Server Deployment

This project contains example Python/Paramiko scripts for SSH operations. Use these as templates for server management tasks.

**Key Scripts** (in `scripts/` folder, gitignored):
- `scripts/deploy_curl.py` - Deploy module to all server instances via SFTP
- `scripts/check_logs.py` - Check console logs across servers
- `scripts/restart_servers.py` - Restart game servers (requires permission)
- `scripts/claim_netdata.py` / `unclaim_netdata.py` - Netdata Cloud management
- `scripts/check_hlstatsx.py` - Check HLStatsX daemon status

**Server Credentials:**
| Server | Host | User | Password |
|--------|------|------|----------|
| Atlanta | 74.91.121.9 | dodserver | ktp |
| Dallas | 74.91.126.55 | dodserver | ktp |
| Denver | 66.163.114.109 | dodserver | ktp |
| New York | 74.91.123.64 | dodserver | ktp |
| Data | 74.91.112.242 | root | (SSH key) |

See `N:\Nein_\KTP Git Projects\CLAUDE.md` for full paramiko SSH documentation.

## Related Projects
- `N:\Nein_\KTP Git Projects\KTPAMXX` - AMX Mod X fork (provides module API)
- `N:\Nein_\KTP Git Projects\KTPMatchHandler` - Primary consumer (Discord integration)
- `N:\Nein_\KTP Git Projects\KTP DoD Server` - Test server with staged modules
