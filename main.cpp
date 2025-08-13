// main.cpp — "vimified" console IDE (Windows-only, no external libs)
// Build: g++ -std=c++23 -Wall -Wextra -pedantic -O2 main.cpp -o text.exe

#include <windows.h>
#include <conio.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// ============ ANSI helpers ============

static void enable_vt() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);
}

static void clear_screen() { std::cout << "\x1b[2J\x1b[H"; } // clear + home
static void move_cursor(int row, int col) { std::cout << "\x1b[" << (row+1) << ";" << (col+1) << "H"; }
static void invert_on() { std::cout << "\x1b[7m"; }
static void invert_off(){ std::cout << "\x1b[0m"; }

static void get_console_size(int& rows, int& cols) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
    rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    cols = info.srWindow.Right  - info.srWindow.Left + 1;
    if (rows < 4) rows = 24;
    if (cols < 20) cols = 80;
}

// ============ file helpers ============
static inline std::optional<std::string> read_text_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::nullopt;
    std::ostringstream oss; oss << ifs.rdbuf();
    if (!ifs.good() && !ifs.eof()) return std::nullopt;
    return oss.str();
}
static inline bool write_text_file(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}
static inline std::string trim_copy(std::string s) {
    auto issp = [](unsigned char c){ return std::isspace(c)!=0; };
    auto b = std::find_if_not(s.begin(), s.end(), issp);
    auto e = std::find_if_not(s.rbegin(), s.rend(), issp).base();
    if (b >= e) return {};
    return {b, e};
}
static inline std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> v;
    std::string t;
    while (iss >> t) {
        v.push_back(t);
    }
    return v;
}
static inline std::string json_escape(std::string_view s) {
    std::string out; out.reserve(s.size()+8);
    for (unsigned char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[7]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
                else out += static_cast<char>(c);
        }
    }
    return out;
}

// ============ token analytics ============

struct TokenStats {
    std::size_t chars{0}, lines{0}, tokens{0}, unique_tokens{0};
    double ttr{0.0}, avg_token_len{0.0}, char_entropy{0.0}, token_entropy{0.0};
    std::size_t digits{0}, letters{0}, whitespace{0}, punctuation{0};
    std::unordered_map<std::string, std::size_t> freq;
};

static std::vector<std::string> tokenize_words(const std::string& s) {
    static const std::regex re(R"([A-Za-z0-9_]+)");
    std::vector<std::string> out;
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) out.push_back(it->str());
    return out;
}

static double shannon_entropy(const std::unordered_map<char, std::size_t>& counts, std::size_t total) {
    if (!total) return 0.0;
    double H = 0.0;
    for (auto& [c, n] : counts) {
        if (n) {
            double p = double(n) / double(total);
            H -= p * std::log2(p);
        }
    }
    return H;
}

static double shannon_entropy_tokens(const std::unordered_map<std::string, std::size_t>& counts, std::size_t total) {
    if (!total) return 0.0;
    double H = 0.0;
    for (auto& [t, n] : counts) {
        if (n) {
            double p = double(n) / double(total);
            H -= p * std::log2(p);
        }
    }
    return H;
}

static TokenStats compute_token_stats(const std::string& content) {
    TokenStats st{}; st.lines = 1;
    std::unordered_map<char, std::size_t> charfreq;
    for (unsigned char c : content) {
        st.chars++; charfreq[(char)c]++;
        if (c=='\n') st.lines++;
        if (std::isdigit(c)) st.digits++;
        else if (std::isalpha(c)) st.letters++;
        else if (std::isspace(c)) st.whitespace++;
        else st.punctuation++;
    }
    auto toks = tokenize_words(content); st.tokens = toks.size();
    std::size_t totlen=0; for (auto& t: toks){ totlen += t.size(); st.freq[t]++; }
    st.unique_tokens = st.freq.size();
    st.ttr = st.tokens? double(st.unique_tokens)/double(st.tokens) : 0.0;
    st.avg_token_len = st.tokens? double(totlen)/double(st.tokens) : 0.0;
    st.char_entropy  = shannon_entropy(charfreq, st.chars);
    st.token_entropy = shannon_entropy_tokens(st.freq, st.tokens);
    return st;
}

static std::vector<std::pair<std::vector<std::string>, std::size_t>>
top_ngrams(const std::vector<std::string>& toks, std::size_t n, std::size_t topk) {
    std::map<std::vector<std::string>, std::size_t> counts;
    if (!n || toks.size() < n) return {};
    for (std::size_t i = 0; i + n <= toks.size(); ++i) {
        std::vector<std::string> key;
        key.reserve(n);
        for (std::size_t j = 0; j < n; ++j) {
            key.push_back(toks[i + j]);
        }
        counts[key]++;
    }
    std::vector<std::pair<std::vector<std::string>, std::size_t>> vec(counts.begin(), counts.end());
    std::sort(vec.begin(), vec.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (vec.size() > topk) {
        vec.resize(topk);
    }
    return vec;
}

// ============ token composition (alphabet = {1,2,3}) ============

static constexpr std::array<char, 3> SAFE_ALPHABET{ '1','2','3' };

static bool safe_pow_u64(std::uint64_t base,std::uint64_t exp,std::uint64_t& out){
    out=1;
    for(std::uint64_t i=0;i<exp;++i){
        if(out> (std::numeric_limits<std::uint64_t>::max)()/base) return false;
        out*=base;
    }
    return true;
}

static std::string compose_permutations(std::uint64_t len, std::uint64_t limit){
    if(!len || len>10 || !limit) return {};
    std::uint64_t total;
    if(!safe_pow_u64(SAFE_ALPHABET.size(),len,total)) total=limit;
    total = std::min<std::uint64_t>(total, limit);
    std::string out; out.reserve((len+1)*total);
    std::string line(len,'1');
    for(std::uint64_t i=0;i<total;++i){
        std::uint64_t t=i;
        for(int j=int(len)-1;j>=0;--j){
            std::uint64_t idx = t % SAFE_ALPHABET.size();
            line[(std::size_t)j] = SAFE_ALPHABET[(std::size_t)idx];
            t /= SAFE_ALPHABET.size();
        }
        out += line; out += '\n';
    }
    return out;
}

// ============ Editor ============

class Editor {
public:
    explicit Editor(const char* initial): filename_(initial?initial:"untitled.txt") {
        if (initial && std::ifstream(initial).good()) { open_file(filename_); dirty_=false; }
        else buffer_.push_back("");
    }
    int run(){
        enable_vt();
        status_ = "Press ESC for COMMAND mode (:help)";
        for(;;){
            ensure_visible();
            draw();
            if (mode_ == "COMMAND") { if (command_loop()) return 0; }
            else edit_loop();
        }
    }
private:
    // buffer state
    std::vector<std::string> buffer_;
    std::string filename_;
    std::string status_{"ready"};
    std::string mode_{"EDIT"};
    std::string cmdbuf_;
    bool dirty_{false};
    int cur_y_{0}, cur_x_{0};
    int off_y_{0}, off_x_{0};

    // ---- rendering ----
    void ensure_visible(){
        int rows, cols; get_console_size(rows, cols);
        int view_h = std::max(1, rows-1), view_w = std::max(1, cols);
        if (cur_y_ < off_y_) off_y_ = cur_y_;
        if (cur_y_ >= off_y_ + view_h) off_y_ = cur_y_ - view_h + 1;
        if (cur_x_ < off_x_) off_x_ = cur_x_;
        if (cur_x_ >= off_x_ + view_w) off_x_ = cur_x_ - view_w + 1;
        if (off_y_ < 0) off_y_ = 0;
        if (off_x_ < 0) off_x_ = 0;
    }
    void draw(){
        int rows, cols; get_console_size(rows, cols);
        clear_screen();
        for(int y=0; y<rows-1; ++y){
            int by = off_y_ + y;
            if (by < 0 || by >= (int)buffer_.size()) break;
            const std::string& full = buffer_[(std::size_t)by];
            std::string slice;
            if (off_x_ < (int)full.size()) slice = full.substr((std::size_t)off_x_);
            move_cursor(y,0); std::cout << slice;
        }
        // status bar
        std::string dirty = dirty_ ? " [+]" : "";
        std::ostringstream left, right;
        left  << " " << mode_ << " | " << filename_ << dirty
              << " | L" << (cur_y_+1) << ", C" << (cur_x_+1) << " ";
        right << " " << status_ << " ";
        std::string L = left.str(), R = right.str();
        int fill = cols - (int)L.size() - (int)R.size(); if (fill<0) fill=0;
        move_cursor(rows-1, 0); invert_on();
        std::cout << L << std::string(fill,' ') << R;
        invert_off();
        // cursor
        int dy = cur_y_ - off_y_, dx = cur_x_ - off_x_;
        if (dy >= 0 && dy < rows-1 && dx >= 0 && dx < cols) move_cursor(dy, dx);
        std::cout.flush();
    }

    // ---- editing ----
    void insert_char(char c){
        if (buffer_.empty()) buffer_.push_back("");
        std::string& line = buffer_[(std::size_t)cur_y_];
        cur_x_ = std::clamp(cur_x_, 0, (int)line.size());
        line.insert((std::size_t)cur_x_, 1, c);
        cur_x_++; dirty_ = true;
    }
    void newline(){
        std::string& line = buffer_[(std::size_t)cur_y_];
        std::string tail = line.substr((std::size_t)cur_x_);
        line.resize((std::size_t)cur_x_);
        buffer_.insert(buffer_.begin()+cur_y_+1, tail);
        cur_y_++; cur_x_=0; dirty_=true;
    }
    void backspace(){
        if (cur_x_>0){
            std::string& line = buffer_[(std::size_t)cur_y_];
            line.erase((std::size_t)(cur_x_-1),1); cur_x_--; dirty_=true;
        } else if (cur_y_>0){
            int prev_len = (int)buffer_[(std::size_t)cur_y_-1].size();
            buffer_[(std::size_t)cur_y_-1] += buffer_[(std::size_t)cur_y_];
            buffer_.erase(buffer_.begin()+cur_y_);
            cur_y_--; cur_x_=prev_len; dirty_=true;
        }
    }
    void del_key(){
        std::string& line = buffer_[(std::size_t)cur_y_];
        if (cur_x_ < (int)line.size()) { line.erase((std::size_t)cur_x_,1); dirty_=true; }
        else if (cur_y_ < (int)buffer_.size()-1) {
            line += buffer_[(std::size_t)cur_y_+1];
            buffer_.erase(buffer_.begin()+cur_y_+1); dirty_=true;
        }
    }
    void edit_loop(){
        int ch = _getch();
        switch (ch){
            case 27: mode_="COMMAND"; status_.clear(); cmdbuf_.clear(); break; // ESC
            case '\r': case '\n': newline(); break;
            case 8: backspace(); break; // Backspace
            case 224: { // extended keys: arrows, Delete, etc.
                int c2 = _getch();
                if (c2 == 72) { // Up
                    if (cur_y_>0) cur_y_--;
                    cur_x_ = std::min<int>(cur_x_, (int)buffer_[(std::size_t)cur_y_].size());
                } else if (c2 == 80) { // Down
                    if (cur_y_<(int)buffer_.size()-1) cur_y_++;
                    cur_x_ = std::min<int>(cur_x_, (int)buffer_[(std::size_t)cur_y_].size());
                } else if (c2 == 75) { // Left
                    if (cur_x_>0) cur_x_--;
                    else if (cur_y_>0){ cur_y_--; cur_x_ = (int)buffer_[(std::size_t)cur_y_].size(); }
                } else if (c2 == 77) { // Right
                    if (cur_x_ < (int)buffer_[(std::size_t)cur_y_].size()) cur_x_++;
                    else if (cur_y_<(int)buffer_.size()-1){ cur_y_++; cur_x_=0; }
                } else if (c2 == 83) { // Delete
                    del_key();
                }
                break;
            }
            case 127: del_key(); break; // Some keyboards send 127 for Delete
            default:
                if (ch== 'h') { if (cur_x_>0) cur_x_--; else if (cur_y_>0){ cur_y_--; cur_x_ = (int)buffer_[(std::size_t)cur_y_].size(); } }
                else if (ch=='j'){ if (cur_y_<(int)buffer_.size()-1) cur_y_++; cur_x_ = std::min<int>(cur_x_, (int)buffer_[(std::size_t)cur_y_].size()); }
                else if (ch=='k'){ if (cur_y_>0) cur_y_--; cur_x_ = std::min<int>(cur_x_, (int)buffer_[(std::size_t)cur_y_].size()); }
                else if (ch=='l'){ if (cur_x_ < (int)buffer_[(std::size_t)cur_y_].size()) cur_x_++; else if (cur_y_<(int)buffer_.size()-1){ cur_y_++; cur_x_=0; } }
                else if (ch >= 32 && ch <= 126) insert_char((char)ch);
                break;
        }
        cur_y_ = std::clamp<int>(cur_y_, 0, (int)buffer_.size()-1);
        cur_x_ = std::clamp<int>(cur_x_, 0, (int)buffer_[(std::size_t)cur_y_].size());
    }

    // ---- command mode ----
    bool command_loop(){
        status_ = ":" + cmdbuf_;
        draw();
        int ch = _getch();
        if (ch == 27) { mode_="EDIT"; status_=""; cmdbuf_.clear(); return false; } // ESC
        if (ch == 8)   { if (!cmdbuf_.empty()) cmdbuf_.pop_back(); return false; } // backspace
        if (ch == '\r' || ch=='\n'){
            bool quit = execute_command(cmdbuf_); cmdbuf_.clear();
            mode_ = "EDIT";
            return quit;
        }
        if (ch==':' && cmdbuf_.empty()) return false; // leading ':' implicit
        if (ch >= 32 && ch <= 126) cmdbuf_.push_back((char)ch);
        return false;
    }

    // ---- open/save ----
    void open_file(const std::string& path){
        auto s = read_text_file(path);
        buffer_.clear();
        if (!s) { buffer_.push_back(""); status_ = "New file: " + path; }
        else {
            std::string text = *s, line; std::istringstream iss(text);
            while (std::getline(iss, line)) { if (!line.empty() && line.back()=='\r') line.pop_back(); buffer_.push_back(line); }
            if (text.size() && text.back()=='\n' && (buffer_.empty() || !buffer_.back().empty())) buffer_.push_back("");
            status_ = "Opened " + path;
        }
        filename_ = path; cur_y_=cur_x_=off_y_=off_x_=0; dirty_=false;
    }
    void save_file(const std::string& path){
        std::ostringstream oss;
        for (std::size_t i=0;i<buffer_.size();++i){ oss << buffer_[i]; if (i+1<buffer_.size()) oss << '\n'; }
        if (write_text_file(path, oss.str())) { filename_ = path; status_ = "Saved " + path; dirty_=false; }
        else status_ = "Error: could not save " + path;
    }

    // ---- shell + compile/run ----
    std::string run_shell_capture(const std::string& cmd){
        std::string full = "powershell -NoProfile -Command \"" + cmd + "\"";
        FILE* pipe = _popen(full.c_str(), "r");
        if (!pipe) return "Error: shell failed.";
        std::string out; char buf[4096];
        for(;;){ std::size_t n=fread(buf,1,sizeof(buf),pipe); if(n>0) out.append(buf,buf+n); if(n<sizeof(buf)) break; }
        _pclose(pipe); return out;
    }
    void command_shell(const std::string& cmd){
        status_ = "Executing: " + (cmd.size()>40? cmd.substr(0,40)+"..." : cmd);
        draw();
        auto out = trim_copy(run_shell_capture(cmd));
        if (out.empty()) status_ = "Command produced no output.";
        else { insert_text_block(out); status_="Command finished."; }
    }
    void command_cpp(){
        status_="Compiling C++23…"; draw();
        char tmpPath[MAX_PATH]; GetTempPathA(MAX_PATH, tmpPath);
        std::string tmpdir = tmpPath;
        std::string src = tmpdir + "vimified_main.cpp";
        std::string exe = tmpdir + "vimified_run.exe";
        // dump buffer to src
        std::ostringstream oss; for (auto& l : buffer_) oss << l << '\n';
        if (!write_text_file(src, oss.str())) { status_="Failed to write temp source."; return; }
        // compile & run
        std::string ccmd = "g++ -std=c++23 \"" + src + "\" -o \"" + exe + "\"";
        auto compile_out = run_shell_capture(ccmd);
        auto run_out     = run_shell_capture("\"" + exe + "\"");
        if (!trim_copy(run_out).empty()) { insert_text_block(trim_copy(run_out)); status_="Program ran (output inserted)."; }
        else if (!trim_copy(compile_out).empty()) { insert_text_block(trim_copy(compile_out)); status_="Compilation failed (diagnostics inserted)."; }
        else status_="Program produced no output.";
    }

    // ---- buffer insertion ----
    void insert_text_block(const std::string& text){
        if (text.empty()) return;
        std::vector<std::string> lines; { std::string s; s.reserve(text.size());
            for (char c: text){ if (c=='\n'){ lines.push_back(s); s.clear(); } else if (c!='\r'){ s.push_back(c);} }
            lines.push_back(s);
        }
        std::string tail = buffer_[(std::size_t)cur_y_].substr((std::size_t)cur_x_);
        buffer_[(std::size_t)cur_y_] =
          buffer_[(std::size_t)cur_y_].substr(0,(std::size_t)cur_x_) + lines.front();
        for (std::size_t i=1;i<lines.size();++i) buffer_.insert(buffer_.begin()+ (cur_y_ + (int)i), lines[i]);
        cur_y_ += (int)lines.size() - 1; cur_x_ = (int)buffer_[(std::size_t)cur_y_].size();
        buffer_[(std::size_t)cur_y_] += tail; dirty_ = true;
    }

    // ---- :tok ----
    void tok_stats(std::optional<std::string> path_opt){
        std::string content;
        if (path_opt){ auto s = read_text_file(*path_opt); if(!s){ status_="tok: cannot open "+*path_opt; return;} content=*s; }
        else { std::ostringstream oss; for (std::size_t i=0;i<buffer_.size();++i){ oss<<buffer_[i]; if(i+1<buffer_.size()) oss<<'\n'; } content=oss.str(); }
        auto st = compute_token_stats(content);
        std::ostringstream js;
        js << "{\n"
           << "  \"lines\": " << st.lines << ",\n"
           << "  \"chars\": " << st.chars << ",\n"
           << "  \"tokens\": " << st.tokens << ",\n"
           << "  \"unique_tokens\": " << st.unique_tokens << ",\n"
           << "  \"type_token_ratio\": " << st.ttr << ",\n"
           << "  \"avg_token_length\": " << st.avg_token_len << ",\n"
           << "  \"char_entropy_bits\": " << st.char_entropy << ",\n"
           << "  \"token_entropy_bits\": " << st.token_entropy << ",\n"
           << "  \"class_counts\": { \"digits\": " << st.digits
           << ", \"letters\": " << st.letters
           << ", \"whitespace\": " << st.whitespace
           << ", \"punctuation\": " << st.punctuation << " }\n"
           << "}";
        insert_text_block(js.str()); status_="Token stats inserted.";
    }
    void tok_ngram(std::size_t N, std::size_t K){
        if (!N){ status_="tok: N must be >=1"; return; }
        std::string content; for (auto& l: buffer_) { content+=l; content+='\n'; }
        auto toks = tokenize_words(content);
        auto res  = top_ngrams(toks, N, K);
        std::ostringstream oss; oss << "Top " << K << " " << N << "-grams:\n";
        for (auto& [ng,cnt] : res){ oss << "  "; for (std::size_t i=0;i<ng.size();++i){ if(i) oss<<' '; oss<<ng[i]; } oss << "  -> " << cnt << "\n"; }
        insert_text_block(oss.str()); status_="N-grams inserted.";
    }
    void tok_export(const std::string& outpath){
        std::ostringstream oss; for (std::size_t i=0;i<buffer_.size();++i){ oss<<buffer_[i]; if(i+1<buffer_.size()) oss<<'\n'; }
        auto st = compute_token_stats(oss.str());
        std::ostringstream js;
        js << "{\n"
           << "  \"file\": \"" << json_escape(outpath) << "\",\n"
           << "  \"lines\": " << st.lines << ",\n"
           << "  \"chars\": " << st.chars << ",\n"
           << "  \"tokens\": " << st.tokens << ",\n"
           << "  \"unique_tokens\": " << st.unique_tokens << ",\n"
           << "  \"type_token_ratio\": " << st.ttr << ",\n"
           << "  \"avg_token_length\": " << st.avg_token_len << ",\n"
           << "  \"char_entropy_bits\": " << st.char_entropy << ",\n"
           << "  \"token_entropy_bits\": " << st.token_entropy << ",\n"
           << "  \"freq\": {";
        bool first=true;
        for (auto& [tok,cnt] : st.freq){ if(!first) js<<","; first=false; js << "\n    \"" << json_escape(tok) << "\": " << cnt; }
        if (!st.freq.empty()) js << "\n  ";
        js << "}\n}\n";
        if (write_text_file(outpath, js.str())) status_="Exported token stats -> "+outpath;
        else status_="tok: export failed";
    }
    void tok_perm(std::uint64_t len, std::uint64_t limit){
        const std::uint64_t MAXL=5000; if (limit>MAXL) limit=MAXL;
        auto out = compose_permutations(len, limit);
        if (out.empty()) { status_="tok: no permutations"; return; }
        insert_text_block(out); status_="Permutations inserted.";
    }

    // ---- execute :commands ----
    bool execute_command(const std::string& raw){
        auto s = trim_copy(raw); if (s.empty()){ status_.clear(); return false; }
        auto parts = split_ws(s); auto cmd = parts.empty()? std::string() : parts[0];

        if (cmd=="q"){ if (dirty_) status_="Unsaved changes! Use :q! to force quit."; else return true; }
        else if (cmd=="q!") return true;
        else if (cmd=="w"){ std::string path = (parts.size()>=2)? parts[1] : filename_; save_file(path); }
        else if (cmd=="o"){
            if (parts.size()<2) status_="Usage: :o <filename>";
            else if (dirty_) status_="Unsaved changes! Save with :w first.";
            else open_file(parts[1]);
        }
        else if (cmd=="help"){ show_help(); }
        else if (cmd=="cpp"){ command_cpp(); }
        else if (!cmd.empty() && cmd[0]=='!'){
            std::string rest = s.substr(1); if (!rest.empty() && rest[0]==' ') rest.erase(0,1); command_shell(rest);
        }
        else if (cmd=="tok"){
            if (parts.size()==1) status_="tok: usage -> :tok stats|ngram|export|perm ...";
            else if (parts[1]=="stats"){ if (parts.size()>=3) tok_stats(parts[2]); else tok_stats(std::nullopt); }
            else if (parts[1]=="ngram"){ std::size_t N = parts.size()>=3? (std::size_t)std::stoul(parts[2]) : 2;
                                         std::size_t K = parts.size()>=4? (std::size_t)std::stoul(parts[3]) : 20; tok_ngram(N,K); }
            else if (parts[1]=="export"){ if (parts.size()<3) status_="tok: export <file.json>"; else tok_export(parts[2]); }
            else if (parts[1]=="perm"){ if (parts.size()<4) status_="tok: perm <len> <limit>";
                                        else tok_perm((std::uint64_t)std::stoull(parts[2]), (std::uint64_t)std::stoull(parts[3])); }
            else status_="tok: unknown subcommand";
        }
        else status_="Unknown command: " + cmd;
        return false;
    }

    void show_help(){
        int rows, cols; get_console_size(rows, cols); clear_screen();
        std::vector<std::string> lines = {
            "--- vimified (C++23, Windows console) Help ---",
            "",
            "MODES",
            "  EDIT:    Type to insert text.",
            "  COMMAND: ESC then type ':' commands.",
            "",
            "MOVE (arrows or h/j/k/l), Backspace/Delete, Enter",
            "",
            "COMMANDS",
            "  :w [file]           Save",
            "  :o <file>           Open (warns if unsaved)",
            "  :q | :q!            Quit / Force quit",
            "  :! <cmd>            Run shell and insert output",
            "  :cpp                Compile & run buffer with g++ -std=c++23",
            "  :tok stats [f]      Token stats (buffer or file)",
            "  :tok ngram N [K]    Top-K N-grams (default K=20)",
            "  :tok export f.json  Save JSON stats for buffer",
            "  :tok perm L M       First M permutations length L (alphabet {1,2,3})",
            "",
            "Press any key…"
        };
        for (int i=0;i<(int)lines.size() && i<rows-1; ++i){ move_cursor(i,2); std::cout << lines[(size_t)i]; }
        std::cout.flush(); _getch();
    }
};

// ============ main ============

int main(int argc, char** argv){
    const char* initial = (argc>=2)? argv[1] : nullptr;
    Editor ed(initial);
    return ed.run();
}
