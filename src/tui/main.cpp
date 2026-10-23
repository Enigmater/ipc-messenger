#include "client_core/client.hpp"
#include <ncursesw/curses.h>
#include <unistd.h>
#include <csignal>
#include <clocale>
#include <cwchar>
#include <iostream>
#include <algorithm>
#include <ctime>
#include <climits>
#include <termios.h>
#include <array>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
std::wstring wide(const std::string& text) {
    std::wstring result;
    std::mbstate_t state{};
    for (std::size_t i = 0; i < text.size();) {
        wchar_t ch{};
        auto n = std::mbrtowc(&ch, text.data() + i, text.size() - i, &state);
        if (n == static_cast<std::size_t>(-1) || n == static_cast<std::size_t>(-2)) {
            state = {}; ch = L'?'; n = 1;
        } else if (n == 0) n = 1;
        result.push_back(ch == L'\n' ? L'\n' : (wcwidth(ch) < 0 ? L'?' : ch));
        i += n;
    }
    return result;
}
std::string utf8(const std::wstring& text) {
    std::string result; std::mbstate_t state{}; char bytes[MB_LEN_MAX];
    for (auto ch : text) {
        auto n = std::wcrtomb(bytes, ch, &state);
        if (n == static_cast<std::size_t>(-1)) throw std::runtime_error("Invalid input encoding");
        result.append(bytes, n);
    }
    return result;
}
void put(int y, int x, const std::wstring& text, int width) {
    int used = 0;
    for (auto ch : text) {
        int cells = std::max(0, wcwidth(ch));
        if (ch == L'\n' || used + cells > width) break;
        mvaddnwstr(y, x + used, &ch, 1); used += cells;
    }
}
struct Terminal {
    bool colors{};
    short user_color(std::uint32_t uid) const {
        return colors ? static_cast<short>(2 + uid % 6) : 0;
    }
    Terminal(bool color) {
        if (!initscr()) throw std::runtime_error("Cannot initialize terminal");
        cbreak(); noecho(); keypad(stdscr, TRUE); timeout(30);
        termios settings{};
        if (tcgetattr(STDIN_FILENO, &settings) == 0) {
            settings.c_iflag &= static_cast<tcflag_t>(~IXON);
            tcsetattr(STDIN_FILENO, TCSANOW, &settings);
        }
        def_prog_mode();
        // Keep Ctrl+C as a signal and allow Ctrl+Q to reach the application.
        intrflush(stdscr, FALSE);
        if (color && has_colors() && start_color() != ERR && COLOR_PAIRS >= 8) {
            const short background = use_default_colors() == ERR ? COLOR_BLACK : -1;
            colors = init_pair(1, COLOR_CYAN, background) != ERR;
            constexpr std::array<short, 6> palette{
                COLOR_CYAN, COLOR_GREEN, COLOR_YELLOW, COLOR_MAGENTA, COLOR_BLUE, COLOR_RED};
            for (std::size_t i = 0; i < palette.size(); ++i)
                if (init_pair(static_cast<short>(2 + i), palette[i], background) == ERR) colors = false;
        }
    }
    ~Terminal() { endwin(); }
};
}
int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
    std::string socket = "/tmp/ipcm-server.sock"; bool ascii = false, color = true;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) socket = argv[++i];
        else if (arg == "--ascii") ascii = true;
        else if (arg == "--no-color") color = false;
        else if (arg == "--help") {
            std::cout << "messenger-tui [--socket PATH] [--ascii] [--no-color]\n"; return 0;
        } else { std::cerr << "Unknown or incomplete option: " << arg << '\n'; return 1; }
    }
    try {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
            throw std::runtime_error("An interactive terminal is required");
        ipcm::client::Client client(socket);
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop); std::signal(SIGHUP, stop);
        Terminal terminal(color);
        color = terminal.colors;
        std::map<std::uint32_t, std::wstring> drafts;
        std::size_t selected = 0, scroll = 0, cursor = 0;
        bool users_focus = true, help = false;
        std::string notice;
        std::vector<std::uint32_t> ids;
        while (!stopped) {
            const auto previous = ids.empty() ? std::optional<std::uint32_t>{} : ids[selected];
            client.update();
            ids.clear();
            for (const auto& [id, user] : client.users) { (void)user; ids.push_back(id); }
            if (previous) {
                auto found = std::find(ids.begin(), ids.end(), *previous);
                if (found != ids.end()) selected = static_cast<std::size_t>(found - ids.begin());
            }
            if (!ids.empty()) selected = std::min(selected, ids.size() - 1);
            auto* user = ids.empty() ? nullptr : &client.users.at(ids[selected]);
            auto& draft = drafts[ids.empty() ? 0 : ids[selected]];
            cursor = std::min(cursor, draft.size());
            if (user && !users_focus) user->unread = 0;
            int rows{}, cols{}; getmaxyx(stdscr, rows, cols);
            erase(); curs_set(0);
            if (rows < 10 || cols < 40) {
                put(0, 0, L"Please resize terminal to at least 40 x 10", cols - 1);
            } else {
                bool compact = cols < 80;
                int split = compact ? 0 : 25;
                attron(A_BOLD); if (color) attron(COLOR_PAIR(1));
                put(0, 1, wide("Local Messenger | " + client.status), cols - 2);
                attroff(A_BOLD); if (color) attroff(COLOR_PAIR(1));
                if (!compact || users_focus) {
                    put(2, 1, L"Users (Tab: switch)", compact ? cols - 2 : split - 2);
                    int height = (rows - 7) / 2;
                    auto start = selected >= static_cast<std::size_t>(height) ? selected - static_cast<std::size_t>(height) + 1 : 0;
                    for (auto i = start; i < ids.size() && i < start + static_cast<std::size_t>(height); ++i) {
                        const auto& u = client.users.at(ids[i]);
                        auto label = (u.online ? (ascii ? L"+ " : L"● ") : (ascii ? L"- " : L"○ ")) + wide(u.name);
                        if (u.unread) label += L" (" + std::to_wstring(u.unread) + L")";
                        if (i == selected) attron(A_REVERSE);
                        const auto pair = terminal.user_color(ids[i]);
                        if (pair) attron(COLOR_PAIR(pair));
                        const int y = 3 + 2 * static_cast<int>(i - start);
                        put(y, 1, label, compact ? cols - 2 : split - 2);
                        if (pair) attroff(COLOR_PAIR(pair));
                        attroff(A_REVERSE);
                        if (!u.history.empty()) {
                            const auto& last = u.history.back();
                            auto stamp = static_cast<std::time_t>(last.timestamp / 1000);
                            char time[16]{}; std::strftime(time, sizeof(time), "%H:%M ", std::localtime(&stamp));
                            attron(A_DIM);
                            put(y + 1, 2, wide(std::string(time) + last.text), compact ? cols - 3 : split - 3);
                            attroff(A_DIM);
                        }
                    }
                }
                if (!compact) for (int y = 2; y < rows - 4; ++y) put(y, split, ascii ? L"|" : L"│", 1);
                if (!compact || !users_focus) {
                    const int x = split + 2, width = cols - x - 2;
                    put(2, x, user ? L"Chat: " + wide(user->name) : L"Waiting for users...", width);
                    if (user) {
                        const auto pair = terminal.user_color(ids[selected]);
                        if (pair) attron(COLOR_PAIR(pair));
                        put(2, x + 6, wide(user->name), width - 6);
                        if (pair) attroff(COLOR_PAIR(pair));
                    }
                    struct Line { std::wstring text; std::wstring sender; short pair{}; };
                    std::vector<Line> lines;
                    if (user) for (const auto& message : user->history) {
                        auto stamp = static_cast<std::time_t>(message.timestamp / 1000);
                        char time[16]{}; std::strftime(time, sizeof(time), "%H:%M", std::localtime(&stamp));
                        lines.push_back({wide(std::string(time) + " " + message.sender + " [" + message.state + "]"),
                            wide(message.sender), terminal.user_color(message.outgoing ? client.uid : ids[selected])});
                        std::wstring line = L"  "; int cells = 2;
                        for (auto ch : wide(message.text)) {
                            int n = std::max(0, wcwidth(ch));
                            if (ch == L'\n' || cells + n > width) {
                                lines.push_back({line, {}, 0}); line = L"  "; cells = 2;
                                if (ch == L'\n') continue;
                            }
                            line += ch; cells += n;
                        }
                        lines.push_back({line, {}, 0});
                    }
                    auto height = static_cast<std::size_t>(rows - 7);
                    scroll = std::min(scroll, lines.size() > height ? lines.size() - height : 0);
                    auto end = lines.size() - scroll, start = end > height ? end - height : 0;
                    for (auto i = start; i < end; ++i) {
                        const auto& line = lines[i];
                        const int y = 3 + static_cast<int>(i - start);
                        put(y, x, line.text, width);
                        if (line.pair) {
                            attron(COLOR_PAIR(line.pair));
                            put(y, x + 6, line.sender, width - 6);
                            attroff(COLOR_PAIR(line.pair));
                        }
                    }
                }
                for (int x = 0; x < cols; ++x) put(rows - 4, x, ascii ? L"-" : L"─", 1);
                const int input_width = cols - 4;
                auto visible = cursor; int cells = 0;
                while (visible > 0 && cells + std::max(0, wcwidth(draft[visible - 1])) < input_width * 2 - 2) {
                    --visible; cells += std::max(0, wcwidth(draft[visible]));
                }
                put(rows - 3, 1, L">", 1);
                int input_row = 0, input_col = 0, cursor_row = 0, cursor_col = 0;
                for (auto i = visible; i <= draft.size(); ++i) {
                    int width = i < draft.size() ? std::max(0, wcwidth(draft[i])) : 1;
                    if (input_col + width > input_width) { ++input_row; input_col = 0; }
                    if (i == cursor) { cursor_row = input_row; cursor_col = input_col; }
                    if (input_row >= 2 || i == draft.size()) break;
                    put(rows - 3 + input_row, 3 + input_col, std::wstring(1, draft[i]), width);
                    input_col += width;
                }
                put(1, 1, wide(notice), cols - 2);
                put(rows - 1, 0, help ? L"Up/Down users | Ctrl+N/P chats | PgUp/PgDn history | F5 refresh | Esc back" :
                    L"Enter: open/send | Tab: panel | F1: help | Ctrl+Q: quit", cols - 1);
                if (!users_focus) { curs_set(1); move(rows - 3 + std::min(cursor_row, 1), 3 + cursor_col); }
            }
            refresh();
            wint_t key{}; const auto result = get_wch(&key);
            if (result == ERR) continue;
            if (result == KEY_CODE_YES) {
                if (key == KEY_RESIZE) continue;
                if (key == KEY_F(1)) help = !help;
                else if (key == KEY_F(5)) client.refresh();
                else if (key == KEY_PPAGE) scroll += 5;
                else if (key == KEY_NPAGE) scroll = scroll > 5 ? scroll - 5 : 0;
                else if (key == KEY_UP && users_focus && selected) { --selected; scroll = cursor = 0; }
                else if (key == KEY_DOWN && users_focus && selected + 1 < ids.size()) { ++selected; scroll = cursor = 0; }
                else if (key == KEY_LEFT && cursor) --cursor;
                else if (key == KEY_RIGHT && cursor < draft.size()) ++cursor;
                else if (key == KEY_HOME) cursor = 0;
                else if (key == KEY_END) cursor = draft.size();
                else if (key == KEY_DC && cursor < draft.size()) draft.erase(cursor, 1);
                else if (key == KEY_BACKSPACE && cursor) draft.erase(--cursor, 1);
                continue;
            }
            if (key == 17) break;
            if (key == 9) { users_focus = !users_focus; cursor = draft.size(); }
            else if (key == 27) { users_focus = true; help = false; }
            else if (key == 12) clearok(stdscr, TRUE);
            else if (key == 14 && selected + 1 < ids.size()) { ++selected; scroll = cursor = 0; }
            else if (key == 16 && selected) { --selected; scroll = cursor = 0; }
            else if (key == 10 || key == 13) {
                if (users_focus) { users_focus = false; cursor = draft.size(); }
                else if (user && !draft.empty()) {
                    try { client.send(ids[selected], utf8(draft)); draft.clear(); cursor = scroll = 0; notice.clear(); }
                    catch (const std::exception& error) { notice = error.what(); }
                }
            } else if ((key == 127 || key == 8) && cursor) draft.erase(--cursor, 1);
            else if (!users_focus && key >= 32 && draft.size() < 65536) draft.insert(cursor++, 1, static_cast<wchar_t>(key));
        }
        return client.connected ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << "messenger-tui: " << error.what() << '\n'; return 1; }
}
