// c++ -std=c++20 -I lib/typeset/src -I lib/Utf8 \
//   tools/xgf2/test_htmlir.cpp lib/typeset/src/HtmlSax.cpp lib/typeset/src/HtmlIR.cpp \
//   lib/typeset/src/FaceCss.cpp lib/Utf8/Utf8.cpp -o /tmp/test_htmlir && /tmp/test_htmlir

#include "HtmlIR.h"

#include <Utf8.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using ts::Atom;
using ts::AtomKind;

struct Dump {
  std::vector<Atom> atoms;
};

static bool collect(void* ctx, const Atom& a, uint32_t) {
  static_cast<Dump*>(ctx)->atoms.push_back(a);
  return true;
}

static std::vector<Atom> parse(const char* xml) {
  std::string wrapped = xml;
  if (wrapped.find("<html") == std::string::npos) {
    wrapped = std::string("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>") + xml + "</body></html>";
  }
  Dump d;
  ts::AtomSink sink{&d, collect};
  ts::htmlToAtoms(wrapped.c_str(), wrapped.size(), sink, nullptr);
  // drop leading/trailing gaps like lazahata trimGaps
  while (!d.atoms.empty() && (d.atoms.front().kind == AtomKind::Gap || d.atoms.front().kind == AtomKind::Space ||
                              d.atoms.front().kind == AtomKind::ColumnBreak)) {
    d.atoms.erase(d.atoms.begin());
  }
  while (!d.atoms.empty() && (d.atoms.back().kind == AtomKind::Gap || d.atoms.back().kind == AtomKind::Space ||
                              d.atoms.back().kind == AtomKind::ColumnBreak)) {
    d.atoms.pop_back();
  }
  return d.atoms;
}

static void fail(const char* msg) {
  std::fprintf(stderr, "FAIL %s\n", msg);
  std::exit(1);
}

static bool isCh(const Atom& a, uint32_t cp, const char* rubyUtf8) {
  if (a.kind != AtomKind::Ch || a.cp != cp) {
    return false;
  }
  if (!rubyUtf8 || !*rubyUtf8) {
    return a.rubyCount == 0;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(rubyUtf8);
  uint8_t n = 0;
  uint32_t cps[4]{};
  while (*p && n < 4) {
    cps[n++] = utf8NextCodepoint(&p);
  }
  if (n != a.rubyCount) {
    return false;
  }
  for (uint8_t i = 0; i < n; ++i) {
    if (cps[i] != a.ruby[i]) {
      return false;
    }
  }
  return true;
}

int main() {
  {
    auto a = parse("<p><ruby><rb>吞</rb><rt>の</rt></ruby></p>");
    if (a.size() != 1 || !isCh(a[0], 0x541E, "の")) {
      fail("ruby rb");
    }
  }
  {
    auto a = parse("<p><ruby>鷲<rt>わし</rt>摑<rt>づか</rt></ruby></p>");
    if (a.size() != 2 || !isCh(a[0], 0x9DF2, "わし") || !isCh(a[1], 0x6451, "づか")) {
      fail("jukugo");
    }
  }
  {
    auto a = parse("<p>4<span class=\"tcy\">kg</span></p>");
    if (a.size() != 2 || a[0].cp != '4' || a[1].kind != AtomKind::Tcy || a[1].cp != 'k') {
      fail("tcy");
    }
  }
  {
    auto a = parse("<p><img class=\"gaiji\" src=\"x.jpeg\" alt=\"→\"/></p>");
    if (a.size() != 1 || a[0].cp != 0x2192) {
      fail("gaiji alt");
    }
  }
  {
    auto a = parse("<p>\n  アナログ\n</p>");
    if (a.size() != 4) {
      fail("pretty print");
    }
  }
  {
    auto a = parse("<p>vitti 'na crozza</p>");
    bool space = false;
    for (auto& x : a) {
      if (x.kind == AtomKind::Space) {
        space = true;
      }
    }
    if (!space) {
      fail("ascii space");
    }
  }
  {
    // 気 at SAX chunk boundary used to split E6 B0 97 and drop the kanji.
    std::string body;
    for (int i = 0; i < 60; ++i) {
      body += "あ";  // 180 UTF-8 bytes, then 気 straddles the old 184 cap
    }
    body += "気の置けない友人";
    std::string html = "<p>" + body + "</p>";
    auto a = parse(html.c_str());
    bool found = false;
    for (auto& x : a) {
      if (x.kind == AtomKind::Ch && x.cp == 0x6C17) {
        found = true;
      }
    }
    if (!found) {
      fail("気 split across text chunk");
    }
  }
  {
    auto a = parse("<p><ruby>門脇<rt>かどわき</rt></ruby></p>");
    if (a.size() != 1 || a[0].kind != AtomKind::Group || a[0].tcyCount != 2 || a[0].tcy[0] != 0x9580 ||
        a[0].tcy[1] != 0x8107 || a[0].rubyCount != 4 || a[0].ruby[0] != 0x304B || a[0].ruby[1] != 0x3069 ||
        a[0].ruby[2] != 0x308F || a[0].ruby[3] != 0x304D) {
      fail("group ruby 門脇");
    }
  }
  {
    auto a = parse("<p><em>強調</em></p>");
    if (a.size() != 2 || a[0].cp != 0x5F37 || a[1].cp != 0x8ABF || a[0].emphasis != 1 || a[1].emphasis != 1) {
      fail("em sesame");
    }
  }
  {
    auto a = parse("<p><span class=\"em-line\">線</span></p>");
    if (a.size() != 1 || a[0].emphasis != 2) {
      fail("em line");
    }
  }
  {
    auto a = parse("<p><span class=\"em-dot\">点</span></p>");
    if (a.size() != 1 || a[0].emphasis != ts::kEmDot) {
      fail("em dot");
    }
  }
  {
    auto a = parse("<p><span class=\"em-sesame-open\">白</span></p>");
    if (a.size() != 1 || a[0].emphasis != ts::kEmSesameOpen) {
      fail("em sesame open");
    }
  }
  {
    const char* html =
        "<html><head><style>"
        "span.board{border:1px solid #000}"
        "span.side{border-right:1px solid}"
        "span.mark{text-emphasis:filled sesame}"
        "em{text-emphasis:none}"
        "em.em-dot{text-emphasis:circle}"
        "</style></head><body><p id=\"here\">"
        "<span class=\"board\">囲文</span>"
        "<span class=\"side\">下</span>"
        "<span class=\"mark\">点</span>"
        "<em>強</em>"
        "<em class=\"em-dot\">丸</em>"
        "</p><a name=\"sec\">い</a></body></html>";
    ts::FaceRule rules[8]{};
    uint16_t nRules = 0;
    ts::scanStyleTags(html, std::strlen(html), rules, nRules);
    struct Hit {
      std::vector<Atom> atoms;
      char ids[4][40]{};
      int nId = 0;
    } hit;
    auto emit = [](void* ctx, const Atom& atom, uint32_t) {
      static_cast<Hit*>(ctx)->atoms.push_back(atom);
      return true;
    };
    auto anchor = [](void* ctx, const char* id) {
      auto* h = static_cast<Hit*>(ctx);
      if (!id || h->nId >= 4) {
        return;
      }
      std::snprintf(h->ids[h->nId], sizeof(h->ids[0]), "%s", id);
      h->nId++;
    };
    ts::AtomSink sink{&hit, emit, nullptr, rules, nRules, anchor};
    if (!ts::htmlToAtoms(html, std::strlen(html), sink, nullptr)) {
      fail("marks html");
    }
    const Atom* box = nullptr;
    const Atom* box2 = nullptr;
    const Atom* mark = nullptr;
    const Atom* em = nullptr;
    const Atom* dot = nullptr;
    const Atom* side = nullptr;
    for (const Atom& a : hit.atoms) {
      if (a.kind != AtomKind::Ch) {
        continue;
      }
      if (a.cp == 0x56F2) {
        box = &a;
      } else if (a.cp == 0x6587) {
        box2 = &a;
      } else if (a.cp == 0x70B9) {
        mark = &a;
      } else if (a.cp == 0x5F37) {
        em = &a;
      } else if (a.cp == 0x4E38) {
        dot = &a;
      } else if (a.cp == 0x4E0B) {
        side = &a;
      }
    }
    if (!box || !box2 || box->box == 0 || box->box != box2->box) {
      fail("board shares a box id");
    }
    if (!mark || (mark->emphasis & ts::kEmphasisMask) != ts::kEmSesame) {
      fail("text-emphasis sesame");
    }
    if (!em || (em->emphasis & ts::kEmphasisMask) != ts::kEmSesame) {
      fail("bare em ignores css none");
    }
    if (!dot || (dot->emphasis & ts::kEmphasisMask) != ts::kEmDot) {
      fail("class dot wins over css circle");
    }
    if (!side || side->box != 0 || (side->emphasis & ts::kEmphasisMask) != ts::kEmLine) {
      fail("one-sided border is a side line");
    }
    if (hit.nId != 2 || std::strcmp(hit.ids[0], "here") != 0 || std::strcmp(hit.ids[1], "sec") != 0) {
      fail("anchor callback");
    }
  }
  {
    auto a = parse("<p><img src=\"a.jpg\"/></p>");
    if (!a.empty()) {
      fail("img without alt became a page");
    }
  }
  {
    auto a = parse("<html><body class=\"p-cover\"><img src=\"c.jpg\"/></body></html>");
    if (a.size() != 1 || a[0].kind != AtomKind::PageBreak) {
      fail("cover without a decoder stays a page break");
    }
  }
  {
    struct Bag {
      std::vector<Atom> atoms;
      int hits = 0;
    };
    auto emit = [](void* ctx, const Atom& atom, uint32_t) {
      static_cast<Bag*>(ctx)->atoms.push_back(atom);
      return true;
    };
    auto pic = [](void* ctx, const char* src) -> uint16_t {
      auto* bag = static_cast<Bag*>(ctx);
      ++bag->hits;
      if (!src || std::strcmp(src, "c.jpg") != 0) {
        return 0;
      }
      return 4;
    };
    Bag bag;
    const char* html = "<html><body class=\"p-cover\"><img src=\"c.jpg\"/></body></html>";
    ts::AtomSink sink{&bag, emit, pic};
    ts::htmlToAtoms(html, std::strlen(html), sink, nullptr);
    if (bag.hits != 1 || bag.atoms.size() != 1 || bag.atoms[0].kind != AtomKind::Picture ||
        bag.atoms[0].cp != (4u | ts::kPictureForcePage) || bag.atoms[0].startEm != 0 || bag.atoms[0].tcyCount != 0) {
      fail("cover picture");
    }
    bag.atoms.clear();
    bag.hits = 0;
    html = "<html><body class=\"p-cover\"><img class=\"gaiji\" src=\"gaiji-cid13803.png\" alt=\"〓\"/></body></html>";
    ts::htmlToAtoms(html, std::strlen(html), sink, nullptr);
    if (bag.hits != 0 || bag.atoms.size() != 1 || bag.atoms[0].kind != AtomKind::Ch || bag.atoms[0].cp != 0x20B9F) {
      fail("cover gaiji stayed a bitmap");
    }
    bag.atoms.clear();
    bag.hits = 0;
    html = "<html><body><p><img src=\"a.jpg\"/></p></body></html>";
    ts::htmlToAtoms(html, std::strlen(html), sink, nullptr);
    if (bag.hits != 1) {
      fail("inline image skipped the picture decoder");
    }
    for (const Atom& a : bag.atoms) {
      if (a.kind == AtomKind::Picture) {
        fail("inline image with no id became a picture");
      }
    }
  }
  {
    auto a = parse("<p><img alt=\"図\"/></p>");
    if (a.size() != 1 || a[0].kind != AtomKind::Ch || a[0].cp != 0x56F3) {
      fail("img alt");
    }
  }
  {
    auto a = parse("<p><img class=\"gaiji\" src=\"gaiji-cid13803.png\" alt=\"〓\"/></p>");
    if (a.size() != 1 || a[0].cp != 0x20B9F) {
      fail("gaiji cid");
    }
  }
  {
    auto a = parse("<h1>題</h1><p>文</p>");
    if (a.size() != 3 || a[0].cp != 0x984C || a[1].kind != AtomKind::ColumnBreak || a[2].cp != 0x6587) {
      std::fprintf(stderr, "heading atoms %zu\n", a.size());
      for (auto& x : a) {
        std::fprintf(stderr, "  kind=%u cp=%04x\n", static_cast<unsigned>(x.kind), x.cp);
      }
      fail("heading column");
    }
  }
  {
    ts::WritingMode mode = ts::WritingMode::VerticalRl;
    const char* html = "<html class=\"hltr\"><body><p>あ</p></body></html>";
    if (!ts::sniffWritingMode(html, std::strlen(html), mode) || mode != ts::WritingMode::HorizontalTb) {
      fail("sniff hltr");
    }
    mode = ts::WritingMode::HorizontalTb;
    html = "<html><body class=\"vrtl\"><p>あ</p></body></html>";
    if (!ts::sniffWritingMode(html, std::strlen(html), mode) || mode != ts::WritingMode::VerticalRl) {
      fail("sniff vrtl");
    }
    mode = ts::WritingMode::VerticalRl;
    html = "<html><head><style>.calibre{writing-mode:horizontal-tb}</style></head><body><p>あ</p></body></html>";
    if (!ts::sniffWritingMode(html, std::strlen(html), mode) || mode != ts::WritingMode::HorizontalTb) {
      fail("sniff css");
    }
  }
  {
    const char* html =
        "<html><head><style>span.big{font-size:150%} strong{font-weight:bold}</style></head>"
        "<body><p><span class=\"big\">大</span><strong>太</strong></p></body></html>";
    ts::FaceRule rules[8]{};
    uint16_t nRules = 0;
    ts::scanStyleTags(html, std::strlen(html), rules, nRules);
    Dump d;
    ts::AtomSink sink{&d, collect, nullptr, rules, nRules};
    ts::htmlToAtoms(html, std::strlen(html), sink, nullptr);
    const Atom* big = nullptr;
    const Atom* bold = nullptr;
    for (const Atom& a : d.atoms) {
      if (a.kind == AtomKind::Ch && a.cp == 0x5927) {
        big = &a;
      }
      if (a.kind == AtomKind::Ch && a.cp == 0x592A) {
        bold = &a;
      }
    }
    if (!big || big->startEm != 150 || (big->emphasis & ts::kEmphasisBold) != 0) {
      fail("span.big 150%");
    }
    if (!bold || (bold->emphasis & ts::kEmphasisBold) == 0) {
      fail("strong bold");
    }
  }
  std::printf("ok htmlir %s\n", "ruby/tcy/gaiji/group/em/img/heading");
  return 0;
}
