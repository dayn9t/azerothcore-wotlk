/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/// \addtogroup Acored
/// @{
/// \file

#include "CliRunnable.h"
#include "Config.h"
#include "ObjectMgr.h"
#include "World.h"
#include <fmt/core.h>

#if AC_PLATFORM == AC_PLATFORM_WINDOWS
#include <windows.h>
#include <iostream>
#else
#include "Chat.h"
#include "ChatCommand.h"
#include "Log.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <readline/history.h>
#include <readline/readline.h>
#include <thread>
#include <unistd.h>
#endif

static constexpr char CLI_PREFIX[] = "AC> ";

static inline void PrintCliPrefix()
{
    fmt::print(CLI_PREFIX);
}

#if AC_PLATFORM != AC_PLATFORM_WINDOWS
namespace Acore::Impl::Readline
{
    static std::vector<std::string> vec;
    char* cli_unpack_vector(char const*, int state)
    {
        static std::size_t i=0;
        if (!state)
            i = 0;
        if (i < vec.size())
            return strdup(vec[i++].c_str());
        else
            return nullptr;
    }

    char** cli_completion(char const* text, int /*start*/, int /*end*/)
    {
        ::rl_attempted_completion_over = 1;
        vec = Acore::ChatCommands::GetAutoCompletionsFor(CliHandler(nullptr,nullptr), text);
        return ::rl_completion_matches(text, &cli_unpack_vector);
    }

    int cli_hook_func()
    {
           if (World::IsStopped())
               ::rl_done = 1;
           return 0;
    }
}
#endif

void utf8print(void* /*arg*/, std::string_view str)
{
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    fmt::print("{}", str);
#else
{
    fmt::print("{}", str);
    fflush(stdout);
}
#endif
}

void commandFinished(void*, bool /*success*/)
{
    PrintCliPrefix();
    fflush(stdout);
}

#ifdef linux
// Non-blocking keypress detector, when return pressed, return 1, else always return 0
int kb_hit_return()
{
    struct timeval tv;
    fd_set fds;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    select(STDIN_FILENO+1, &fds, nullptr, nullptr, &tv);
    return FD_ISSET(STDIN_FILENO, &fds);
}
#endif

/// %Thread start
void CliThread()
{
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    // Set console code pages to UTF-8
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    // print this here the first time
    // later it will be printed after command queue updates
    PrintCliPrefix();
#else
    ::rl_attempted_completion_function = &Acore::Impl::Readline::cli_completion;
    {
        static char BLANK = '\0';
        ::rl_completer_word_break_characters = &BLANK;
    }
    ::rl_event_hook = &Acore::Impl::Readline::cli_hook_func;

    // The interactive CLI needs a real terminal. Under systemd/docker, stdin is
    // /dev/null: readline() returns NULL immediately without setting feof, which
    // (before this guard) busy-looped the "AC> " prompt and flooded stdout at
    // ~3GB/h. For non-interactive stdin we fall back to blocking getline (mirrors
    // the Windows redirected-input path) and idle on EOF instead of spamming.
    bool const cliInteractive = isatty(STDIN_FILENO) != 0;
    bool cliInputEof = false;
    if (!cliInteractive)
        LOG_INFO("server.worldserver", "Stdin is not a terminal — interactive console disabled (no 'AC> ' prompt).");
#endif

    if (sConfigMgr->GetOption<bool>("BeepAtStart", true))
        printf("\a"); // \a = Alert

#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    if (sConfigMgr->GetOption<bool>("FlashAtStart", true))
    {
        FLASHWINFO fInfo;
        fInfo.cbSize = sizeof(FLASHWINFO);
        fInfo.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        fInfo.hwnd = GetConsoleWindow();
        fInfo.uCount = 0;
        fInfo.dwTimeout = 0;
        FlashWindowEx(&fInfo);
    }

    // Get console input handle once for reading commands
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdIn == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("server.worldserver", "Failed to get console input handle");
        return;
    }
#endif

    ///- As long as the World is running (no World::m_stopEvent), get the command line and handle it
    while (!World::IsStopped())
    {
        fflush(stdout);

        std::string command;

#if AC_PLATFORM == AC_PLATFORM_WINDOWS

        static bool checkedConsole = false;
        static bool isRealConsole = false;

        if (!checkedConsole)
        {
            DWORD mode = 0;
            isRealConsole = GetConsoleMode(hStdIn, &mode);
            checkedConsole = true;
        }

        if (isRealConsole)
        {
            // ===== Real Windows Console =====
            wchar_t commandbuf[256];
            DWORD charsRead = 0;

            if (ReadConsoleW(hStdIn, commandbuf,
                sizeof(commandbuf) / sizeof(wchar_t) - 1,
                &charsRead, nullptr))
            {
                if (charsRead > 0)
                {
                    commandbuf[charsRead] = L'\0';
                    if (!WStrToUtf8(commandbuf, charsRead, command))
                    {
                        PrintCliPrefix();
                        continue;
                    }
                }
            }
        }
        else
        {
            // ===== Redirected input (pipe) =====
            if (!std::getline(std::cin, command))
            {
                World::StopNow(SHUTDOWN_EXIT_CODE);
                break;
            }
        }

#else
        if (!cliInteractive)
        {
            // Redirected/non-interactive stdin (systemd /dev/null, or piped commands).
            // getline blocks for real input and never prints a prompt, so no flood.
            if (cliInputEof)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            if (!std::getline(std::cin, command))
            {
                // stdin EOF (pipe drained or /dev/null). It won't un-EOF, so stop
                // reading and idle until shutdown — don't busy-loop and don't shut
                // the world down just because the console detached.
                cliInputEof = true;
                continue;
            }
        }
        else
        {
            char* command_str = readline(CLI_PREFIX);
            ::rl_bind_key('\t', ::rl_complete);
            if (command_str != nullptr)
            {
                command = command_str;
                free(command_str);
            }
        }
#endif

        if (!command.empty())
        {
            std::size_t nextLineIndex = command.find_first_of("\r\n");
            if (nextLineIndex != std::string::npos)
            {
                if (nextLineIndex == 0)
                {
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
                    PrintCliPrefix();
#endif
                    continue;
                }

                command.erase(nextLineIndex);
            }

            fflush(stdout);
            sWorld->QueueCliCommand(new CliCommandHolder(nullptr, command.c_str(), &utf8print, &commandFinished));
#if AC_PLATFORM != AC_PLATFORM_WINDOWS
            add_history(command.c_str());
#endif
        }
        else if (feof(stdin))
        {
            World::StopNow(SHUTDOWN_EXIT_CODE);
        }
    }
}
