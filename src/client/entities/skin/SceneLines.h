#pragma once
// A cutscene's subtitle without CJ's name, for the scenes a skin's voice plays in (SceneVoice.cpp): nobody names him
// there, the name is wiped from the sound, and the line is shown the same way.
//  * A call to him goes whole: "Hey, what's up, CJ?" is "Hey, what's up?", "CJ, help me out here!" is "Help me out
//    here!", "I know CJ, I know." is "I know, I know.", a line that was his name alone is empty.
//  * A name the sentence needs leaves the pause the sound has there: "I'm gonna speak to Carl." is "I'm gonna speak
//    to...", "The name's Carl Johnson, sir." is "The name's..., sir.", "Carl's here." is "...here.".
// The rules are the tests' el_scene.py's (strip_name), one pass each from the left as a regular expression's replace
// goes, and are checked against it on every line of the game's English text (the tests' unit/test_scene_lines.cpp):
// change both together. The names are the English ones and those of SanLTD's Russian text (RussianName); a line of
// another translation that spells him otherwise stays as it is.
// No game code here: the unit test builds this header alone.
#include <cctype>
#include <string>
#include <string_view>

namespace coop::skin {

namespace scene_lines {

constexpr char kName = '\x01';   // where a name stood
constexpr char kPause = '\x02';  // the pause a name leaves in a line, "..." in the end: no full stop to the rules
constexpr char kOwned = '\x03';  // a name with an ending ("Carl's", SanLTD's "CJ'ya"): never a call, always a pause

inline bool Word(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
inline bool Space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
inline bool Stop(char c) { return c == '.' || c == '!' || c == '?'; }
inline bool Named(char c) { return c == kName || c == kOwned; }
inline bool Has(const std::string& s, size_t i, std::string_view word) { return s.compare(i, word.size(), word) == 0; }
inline bool Ends(const std::string& s, size_t i) { return i >= s.size() || !Word(s[i]); }  // a word ends before i
inline size_t Spaces(const std::string& s, size_t i) {  // past the spaces from i
    while (i < s.size() && Space(s[i])) ++i;
    return i;
}
// What a sentence starts after, at p: the line's start, a subtitle's mark, a new line, a speaker's dash (at a line's
// start only: "We're all the same, Carl - you, me." got a capital after its dash), a full stop. Its length each way
// it can be read, -1 when p is none of them
inline int Start(const std::string& s, size_t p, int way) {
    bool mark = Has(s, p, "~z~") || Has(s, p, "~n~"), spaced = p + 1 < s.size() && s[p + 1] == ' ';
    if (way == 0) return p == 0 ? 0 : -1;
    if (way == 1) return mark ? 3 : -1;
    if (way == 2) return spaced && s[p] == '-' && (p == 0 || (p >= 3 && (Has(s, p - 3, "~z~") || Has(s, p - 3, "~n~")))) ? 2 : -1;
    return spaced && Stop(s[p]) ? 2 : -1;
}
constexpr int kStarts = 4;
// a word after his name that makes him the sentence's subject, not the one called ("Carl here is opening a garage")
inline bool Verb(const std::string& s, size_t i) {
    for (std::string_view v : {"is", "was", "here", "don't", "doesn't", "didn't", "will", "has", "had", "can", "can't", "drive",
                               "do", "did", "does", "ain't", "got"})
        if (Has(s, i, v) && Ends(s, i + v.size())) return true;
    return false;
}
// a word before his name that needs it ("The name's Carl Johnson, sir."); after any other he is called
inline bool Complement(std::string word) {
    for (char& c : word) c = (char)std::tolower((unsigned char)c);
    for (std::string_view w : {"as", "with", "name's", "it's", "to", "for", "of", "from", "than", "and", "no", "you're", "i'm"})
        if (word == w) return true;
    return false;
}

// Johnson alone, as the brothers' (Sweet's "The Johnson brothers are rolling again."), his mother's ("Aren't you Beverly
// Johnson's boy?") and his own ("I'm Johnson."), is his name too since 13.11 (the user's choice: it was heard in the
// scenes); Sweet Johnson stays Sweet's
inline bool AfterSweet(const std::string& s, size_t i) {
    return i >= 6 && (Has(s, i - 6, "Sweet ") || Has(s, i - 6, "SWEET "));
}

// The Russian text most players have (SanLTD's, read by the game in place of american.gxt) writes Cyrillic with the
// bytes 0x80-0xAF and the Latin letters that look the same, and leaves "CJ" as it is. His name there, as its 872 lines
// that name him have it: Karl (Kap\x9E, 396 times with its endings -a, -u, -om, -e), Karl Dzhonson, mister Dzhonson,
// and since 13.11 Dzhonson alone as the English Johnson (Dzhonsony, Dzhonsonov: the brothers, the family) but Svit's.
// Mister alone is anyone's, and Karlito or Karlos are other words (the word must end).
inline int Ending(const std::string& s, size_t i) {  // a case ending and the word's end: its length, -1 when the word goes on
    for (std::string_view e : {"o\xAF", "a", "y", "e", ""})
        if (Has(s, i, e) && Ends(s, i + e.size())) return (int)e.size();
    return -1;
}
inline size_t RussianName(const std::string& s, size_t i) {  // the length of his name at i, 0: none
    size_t at = i, end = 0;
    bool mister = false;
    for (std::string_view m : {"\xAF\x9C" "c\xA6" "ep", "\x96\x9C" "c\xA6" "ep"}) {  // mister, Mister
        int e = Has(s, at, m) ? Ending(s, at + m.size()) : -1;
        if (e < 0 || at + m.size() + e >= s.size() || s[at + m.size() + e] != ' ') continue;
        at += m.size() + e + 1, mister = true;
        break;
    }
    for (std::string_view k : {"Kap\x9E", "KAP\x87"}) {  // Karl, KARL
        int e = Has(s, at, k) ? Ending(s, at + k.size()) : -1;
        if (e >= 0) end = at + k.size() + e;
    }
    if (end ? end < s.size() && s[end] == ' ' : mister) {  // Dzhonson after either
        size_t from = end ? end + 1 : at;
        for (std::string_view j : {"\x83\x9B" "o\xAE" "co\xAE", "\x83\x84O\xAD" "CO\xAD"}) {  // Dzhonson, DZHONSON
            int e = Has(s, from, j) ? Ending(s, from + j.size()) : -1;
            if (e >= 0) end = from + j.size() + e;
        }
    }
    bool svit = i >= 5 && (Has(s, i - 5, "C\xA2\x9C\xA6 ") || Has(s, i - 5, "C\x8B\x85T "));  // Svit, SVIT
    for (std::string_view j : {"\x83\x9B" "o\xAE" "co\xAE", "\x83\x84O\xAD" "CO\xAD"}) {   // Dzhonson alone
        if (end || mister || svit || !Has(s, i, j)) continue;
        for (std::string_view e : {"o\xA2", "\xA8", "o\xAF", "a", "y", "e", ""})  // -ov, -y, -om, -a, -u, -e
            if (!end && Has(s, i + j.size(), e) && Ends(s, i + j.size() + e.size())) end = i + j.size() + e.size();
    }
    return end ? end - i : 0;
}
// a line of that text: its letters are bytes over 0x7F for a third at least (0.47 over the whole file; another
// language's accents are a few in a hundred). Its commas set a call off on both sides, so a name with none is the
// sentence's own there
inline bool Russian(std::string_view s) {
    int letters = 0, high = 0;
    for (char c : s) letters += Word(c) && !std::isdigit((unsigned char)c) && c != '_', high += (unsigned char)c >= 0x80;
    return high * 3 >= letters && high > 0;
}
// its small letter's capital (b to ya are 0x97-0xAC, their capitals 0x17 lower; n 0xAE and m 0xAF have 0xAD and 0x96),
// the byte itself for any other
inline char RussianCapital(char c) {
    unsigned char u = (unsigned char)c;
    return (char)(u >= 0x97 && u <= 0xAC ? u - 0x17 : u == 0xAE ? 0xAD : u == 0xAF ? 0x96 : u);
}

// One pass: at each place from the left `rule` either takes some of the line (returns how much, having written what
// stands for it) or nothing (0), and the pass goes on after what was taken.
template <typename Rule>
inline std::string Pass(const std::string& s, Rule rule) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        size_t taken = rule(s, i, out);
        if (taken) i += taken;
        else out += s[i++];
    }
    return out;
}

}  // namespace scene_lines

inline std::string WithoutName(std::string_view line) {
    using namespace scene_lines;
    using Line = const std::string&;
    std::string s(line);
    bool named = false, russian = Russian(line);
    s = Pass(s, [&](Line t, size_t i, std::string& out) -> size_t {
        if (i && Word(t[i - 1])) return 0;
        size_t n = 0;
        for (std::string_view name : {"Mr. Johnson", "Carl Johnson", "CARL JOHNSON", "CJ", "CARL", "Carl"})
            if (!n && Has(t, i, name) && Ends(t, i + name.size())) n = name.size();
        for (std::string_view name : {"Johnson", "JOHNSON"})
            if (!n && Has(t, i, name) && Ends(t, i + name.size()) && !AfterSweet(t, i)) n = name.size();
        if (!n) n = RussianName(t, i);
        if (!n) return 0;
        size_t ending = i + n + 1;  // "'s": an apostrophe and up to three letters
        while (i + n < t.size() && t[i + n] == '\'' && ending < t.size() && Word(t[ending])) ++ending;
        bool owned = ending > i + n + 1 && ending <= i + n + 4;
        out += owned ? kOwned : kName, named = true;
        return owned ? ending - i : n;
    });
    if (!named) return std::string(line);
    // "Carl Johnson - CJ."
    s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
        size_t j = t[i] == kName ? Spaces(t, i + 1) : t.size();
        if (j >= t.size() || t[j] != '-') return 0;
        j = Spaces(t, j + 1);
        if (j >= t.size() || t[j] != kName) return 0;
        out += kName;
        return j + 1 - i;
    });
    // "Carl!" said alone, and "CJ? CJ!" each
    for (std::string was; was != s;) {
        was = s;
        s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
            for (int way = 0; way < kStarts; ++way) {
                int len = Start(t, i, way);
                if (len < 0 || i + len >= t.size() || t[i + len] != kName) continue;
                size_t j = Spaces(t, i + len + 1), stops = j;
                while (stops < t.size() && Stop(t[stops])) ++stops;
                if (stops == j && j < t.size()) continue;
                out.append(t, i, len);
                return Spaces(t, stops) - i;
            }
            return 0;
        });
    }
    // "what's up, CJ?" -> "what's up?"
    s = Pass(s, [](Line t, size_t i, std::string&) -> size_t {
        size_t j = t[i] == ',' ? Spaces(t, i + 1) : t.size();
        if (j >= t.size() || t[j] != kName) return 0;
        size_t k = Spaces(t, j + 1);
        return k >= t.size() || std::string_view(",.!?~:;-").find(t[k]) != std::string_view::npos ? j + 1 - i : 0;
    });
    // "Hey, Carl my boy!" -> "Hey, my boy!"
    if (!russian)
        s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
            size_t j = t[i] == ',' ? Spaces(t, i + 1) : t.size();
            size_t k = j < t.size() && t[j] == kName ? Spaces(t, j + 1) : j + 1;
            if (k == j + 1 || k >= t.size() || !Word(t[k]) || Verb(t, k)) return 0;
            out += ", ";
            return k - i;
        });
    // "CJ, help me" -> "help me", "Carl - this is" -> "this is"
    s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
        for (int way = 0; way < kStarts; ++way) {
            int len = Start(t, i, way);
            if (len < 0 || i + len >= t.size() || t[i + len] != kName) continue;
            size_t j = Spaces(t, i + len + 1);
            if (j >= t.size() || (t[j] != ',' && t[j] != '-')) continue;
            out.append(t, i, len);
            return Spaces(t, j + 1) - i;
        }
        return 0;
    });
    // "CJ you gotta come" -> "you gotta come"
    if (!russian)
        s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
            for (int way = 0; way < kStarts; ++way) {
                int len = Start(t, i, way);
                if (len < 0 || i + len >= t.size() || t[i + len] != kName) continue;
                size_t k = Spaces(t, i + len + 1);
                if (k == i + len + 1 || k >= t.size() || !Word(t[k]) || Verb(t, k)) continue;
                out.append(t, i, len);
                return k - i;
            }
            return 0;
        });
    // "Hey CARL!" -> "Hey!"; SanLTD's Ey and Yo
    s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
        if (i && Word(t[i - 1])) return 0;
        for (std::string_view word : {"hey", "hi", "yo", "oh", "yeah", "thanks", "\x93\x9D", "\x86oy"}) {
            size_t n = 0;
            while (n < word.size() && i + n < t.size() &&
                   ((unsigned char)word[n] >= 0x80 ? t[i + n] : (char)std::tolower((unsigned char)t[i + n])) == word[n])
                ++n;
            size_t j = n == word.size() ? Spaces(t, i + n) : i + n;
            if (n != word.size() || j == i + n || j >= t.size() || t[j] != kName) continue;
            out.append(t, i, n);
            return j + 1 - i;
        }
        return 0;
    });
    // "Stick with me CJ, we" -> "Stick with me, we"; "The name's Carl Johnson, sir." keeps him for the pause
    if (!russian)
        s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
            auto in = [&](size_t p) { return p < t.size() && (Word(t[p]) || t[p] == '\''); };
            if (!in(i) || (i && in(i - 1))) return 0;
            size_t e = i;
            while (in(e)) ++e;
            size_t j = Spaces(t, e);
            size_t k = j > e && j < t.size() && t[j] == kName ? Spaces(t, j + 1) : t.size();
            if (k >= t.size() || t[k] != ',') return 0;
            out.append(t, i, Complement(t.substr(i, e - i)) ? j + 1 - i : e - i);
            return j + 1 - i;
        });
    // "speak to Carl." -> "speak to...": the pause in the line
    s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
        size_t j = Word(t[i]) ? Spaces(t, i + 1) : i + 1;
        if (j == i + 1 || j >= t.size()) return 0;
        if (t[j] == '-') {
            size_t after = Spaces(t, j + 1);
            if (after == j + 1) return 0;
            j = after;
        }
        if (j >= t.size() || !Named(t[j])) return 0;
        out += t[i], out += kPause;
        return j + 1 - i + (j + 1 < t.size() && t[j + 1] == '.');
    });
    // "shakin' Carl?" -> "shakin'?"
    s = Pass(s, [](Line t, size_t i, std::string&) -> size_t {
        size_t j = Spaces(t, i);
        if (j >= t.size() || t[j] != kName) return 0;
        size_t k = Spaces(t, j + 1);
        return k >= t.size() || std::string_view(".!?,;:").find(t[k]) != std::string_view::npos ? j + 1 - i : 0;
    });
    // "Carl's here" -> "...here"
    s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
        if (!Named(t[i])) return 0;
        out += kPause;
        return Spaces(t, i + 1) - i;
    });
    s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {  // no space after a mark
        size_t j = Start(t, i, 1) == 3 ? Spaces(t, i + 3) : i + 3;
        if (j == i + 3) return 0;
        out.append(t, i, 3);
        return j - i;
    });
    s = Pass(s, [](Line t, size_t i, std::string&) -> size_t {  // nor before a comma or a full stop
        size_t j = Spaces(t, i);
        return j > i && j < t.size() && (t[j] == ',' || Stop(t[j])) ? j - i : 0;
    });
    s = Pass(s, [](Line t, size_t i, std::string&) -> size_t {  // ",." -> "."
        return t[i] == ',' && i + 1 < t.size() && Stop(t[i + 1]) ? 1 : 0;
    });
    s = Pass(s, [&](Line t, size_t i, std::string& out) -> size_t {  // a sentence starts with a capital
        for (int way = 0; way < kStarts; ++way) {
            int len = Start(t, i, way);
            if (len < 0 || i + len >= t.size()) continue;
            char c = t[i + len], capital = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : russian ? RussianCapital(c) : c;
            if (capital == c) continue;
            out.append(t, i, len);
            out += capital;
            return len + 1;
        }
        return 0;
    });
    s = Pass(s, [](Line t, size_t i, std::string& out) -> size_t {  // one space between words
        size_t j = i;
        while (j < t.size() && t[j] == ' ') ++j;
        if (j < i + 2) return 0;
        out += ' ';
        return j - i;
    });
    size_t first = Spaces(s, 0), last = s.size();
    while (last > first && Space(s[last - 1])) --last;
    s = s.substr(first, last - first);
    bool letters = false;  // nothing but marks and stops left: no line. A Russian line's letters are also the bytes over 0x7F
    for (size_t i = 0; i < s.size() && !letters; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (Start(s, i, 1) == 3) i += 2;
        else letters = c < 0x80 ? std::isalpha(c) != 0 : russian;
    }
    if (!letters) return std::string();
    return Pass(s, [](Line t, size_t i, std::string& out) -> size_t {
        if (t[i] != kPause) return 0;
        out += "...";
        return 1;
    });
}

}  // namespace coop::skin
