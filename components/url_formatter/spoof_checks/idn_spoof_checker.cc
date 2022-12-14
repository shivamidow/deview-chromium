// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/url_formatter/spoof_checks/idn_spoof_checker.h"

#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/threading/thread_local_storage.h"
#include "build/build_config.h"
#include "net/base/lookup_string_in_fixed_set.h"
#include "third_party/icu/source/common/unicode/schriter.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/i18n/unicode/regex.h"
#include "third_party/icu/source/i18n/unicode/translit.h"
#include "third_party/icu/source/i18n/unicode/uspoof.h"

namespace url_formatter {

namespace {

class TopDomainPreloadDecoder : public net::extras::PreloadDecoder {
 public:
  using net::extras::PreloadDecoder::PreloadDecoder;
  ~TopDomainPreloadDecoder() override {}

  bool ReadEntry(net::extras::PreloadDecoder::BitReader* reader,
                 const std::string& search,
                 size_t current_search_offset,
                 bool* out_found) override {
    bool is_same_skeleton;
    if (!reader->Next(&is_same_skeleton))
      return false;

    TopDomainEntry top_domain;
    if (!reader->Next(&top_domain.is_top_500))
      return false;

    if (is_same_skeleton) {
      top_domain.domain = search;
    } else {
      bool has_com_suffix = false;
      if (!reader->Next(&has_com_suffix))
        return false;

      for (char c;; top_domain.domain += c) {
        huffman_decoder().Decode(reader, &c);
        if (c == net::extras::PreloadDecoder::kEndOfTable)
          break;
      }
      if (has_com_suffix)
        top_domain.domain += ".com";
    }

    if (current_search_offset == 0) {
      *out_found = true;
      DCHECK(!top_domain.domain.empty());
      result_ = top_domain;
    }
    return true;
  }

  TopDomainEntry matching_top_domain() const { return result_; }

 private:
  TopDomainEntry result_;
};

void OnThreadTermination(void* regex_matcher) {
  delete reinterpret_cast<icu::RegexMatcher*>(regex_matcher);
}

base::ThreadLocalStorage::Slot& DangerousPatternTLS() {
  static base::NoDestructor<base::ThreadLocalStorage::Slot>
      dangerous_pattern_tls(&OnThreadTermination);
  return *dangerous_pattern_tls;
}

// Allow middle dot (U+00B7) only on Catalan domains when between two 'l's, to
// permit the Catalan character ela geminada to be expressed.
// See https://tools.ietf.org/html/rfc5892#appendix-A.3 for details.
bool HasUnsafeMiddleDot(const icu::UnicodeString& label_string,
                        base::StringPiece top_level_domain) {
  int last_index = 0;
  while (true) {
    int index = label_string.indexOf("??", last_index);
    if (index < 0) {
      break;
    }
    DCHECK_LT(index, label_string.length());
    if (top_level_domain != "cat") {
      // Non-Catalan domains cannot contain middle dot.
      return true;
    }
    // Middle dot at the beginning or end.
    if (index == 0 || index == label_string.length() - 1) {
      return true;
    }
    // Middle dot not surrounded by an 'l'.
    if (label_string[index - 1] != 'l' || label_string[index + 1] != 'l') {
      return true;
    }
    last_index = index + 1;
  }
  return false;
}

#include "components/url_formatter/spoof_checks/top_domains/domains-trie-inc.cc"

// All the domains in the above file have 4 or fewer labels.
const size_t kNumberOfLabelsToCheck = 4;

IDNSpoofChecker::HuffmanTrieParams g_trie_params{
    kTopDomainsHuffmanTree, sizeof(kTopDomainsHuffmanTree), kTopDomainsTrie,
    kTopDomainsTrieBits, kTopDomainsRootPosition};

}  // namespace

IDNSpoofChecker::IDNSpoofChecker() {
  UErrorCode status = U_ZERO_ERROR;
  checker_ = uspoof_open(&status);
  if (U_FAILURE(status)) {
    checker_ = nullptr;
    return;
  }

  // At this point, USpoofChecker has all the checks enabled except
  // for USPOOF_CHAR_LIMIT (USPOOF_{RESTRICTION_LEVEL, INVISIBLE,
  // MIXED_SCRIPT_CONFUSABLE, WHOLE_SCRIPT_CONFUSABLE, MIXED_NUMBERS, ANY_CASE})
  // This default configuration is adjusted below as necessary.

  // Set the restriction level to high. It allows mixing Latin with one logical
  // CJK script (+ COMMON and INHERITED), but does not allow any other script
  // mixing (e.g. Latin + Cyrillic, Latin + Armenian, Cyrillic + Greek). Note
  // that each of {Han + Bopomofo} for Chinese, {Hiragana, Katakana, Han} for
  // Japanese, and {Hangul, Han} for Korean is treated as a single logical
  // script.
  // See http://www.unicode.org/reports/tr39/#Restriction_Level_Detection
  uspoof_setRestrictionLevel(checker_, USPOOF_HIGHLY_RESTRICTIVE);

  // Sets allowed characters in IDN labels and turns on USPOOF_CHAR_LIMIT.
  SetAllowedUnicodeSet(&status);

  // Enable the return of auxillary (non-error) information.
  // We used to disable WHOLE_SCRIPT_CONFUSABLE check explicitly, but as of
  // ICU 58.1, WSC is a no-op in a single string check API.
  int32_t checks = uspoof_getChecks(checker_, &status) | USPOOF_AUX_INFO;
  uspoof_setChecks(checker_, checks, &status);

  // Four characters handled differently by IDNA 2003 and IDNA 2008. UTS46
  // transitional processing treats them as IDNA 2003 does; maps U+00DF and
  // U+03C2 and drops U+200[CD].
  deviation_characters_ = icu::UnicodeSet(
      UNICODE_STRING_SIMPLE("[\\u00df\\u03c2\\u200c\\u200d]"), status);
  deviation_characters_.freeze();

  // Latin letters outside ASCII. 'Script_Extensions=Latin' is not necessary
  // because additional characters pulled in with scx=Latn are not included in
  // the allowed set.
  non_ascii_latin_letters_ =
      icu::UnicodeSet(UNICODE_STRING_SIMPLE("[[:Latin:] - [a-zA-Z]]"), status);
  non_ascii_latin_letters_.freeze();

  // The following two sets are parts of |dangerous_patterns_|.
  kana_letters_exceptions_ = icu::UnicodeSet(
      UNICODE_STRING_SIMPLE("[\\u3078-\\u307a\\u30d8-\\u30da\\u30fb-\\u30fe]"),
      status);
  kana_letters_exceptions_.freeze();
  combining_diacritics_exceptions_ =
      icu::UnicodeSet(UNICODE_STRING_SIMPLE("[\\u0300-\\u0339]"), status);
  combining_diacritics_exceptions_.freeze();

  // These Cyrillic letters look like Latin. A domain label entirely made of
  // these letters is blocked as a simplified whole-script-spoofable.
  cyrillic_letters_latin_alike_ = icu::UnicodeSet(
      icu::UnicodeString::fromUTF8("[????????????????????????????????????????????????????]"), status);
  cyrillic_letters_latin_alike_.freeze();

  cyrillic_letters_ =
      icu::UnicodeSet(UNICODE_STRING_SIMPLE("[[:Cyrl:]]"), status);
  cyrillic_letters_.freeze();

  // These characters are, or look like, digits. A domain label entirely made of
  // digit-lookalikes or digits is blocked.
  digits_ = icu::UnicodeSet(UNICODE_STRING_SIMPLE("[0-9]"), status);
  digits_.freeze();
  digit_lookalikes_ = icu::UnicodeSet(
      icu::UnicodeString::fromUTF8("[???????????????????????????????????????????????????????????????????????????????????????]"),
      status);
  digit_lookalikes_.freeze();

  DCHECK(U_SUCCESS(status));
  // This set is used to determine whether or not to apply a slow
  // transliteration to remove diacritics to a given hostname before the
  // confusable skeleton calculation for comparison with top domain names. If
  // it has any character outside the set, the expensive step will be skipped
  // because it cannot match any of top domain names.
  // The last ([\u0300-\u0339] is a shorthand for "[:Identifier_Status=Allowed:]
  // & [:Script_Extensions=Inherited:] - [\\u200C\\u200D]". The latter is a
  // subset of the former but it does not matter because hostnames with
  // characters outside the latter set would be rejected in an earlier step.
  lgc_letters_n_ascii_ = icu::UnicodeSet(
      UNICODE_STRING_SIMPLE("[[:Latin:][:Greek:][:Cyrillic:][0-9\\u002e_"
                            "\\u002d][\\u0300-\\u0339]]"),
      status);
  lgc_letters_n_ascii_.freeze();

  // Latin small letter thorn ("??", U+00FE) can be used to spoof both b and p.
  // It's used in modern Icelandic orthography, so allow it for the Icelandic
  // ccTLD (.is) but block in any other TLD. Also block Latin small letter eth
  // ("??", U+00F0) which can be used to spoof the letter o.
  icelandic_characters_ =
      icu::UnicodeSet(UNICODE_STRING_SIMPLE("[\\u00fe\\u00f0]"), status);
  icelandic_characters_.freeze();

  // Used for diacritics-removal before the skeleton calculation. Add
  // "?? > l; ?? > o; ?? > d" that are not handled by "NFD; Nonspacing mark
  // removal; NFC".
  // TODO(jshin): Revisit "?? > l; ?? > o" mapping.
  UParseError parse_error;
  diacritic_remover_.reset(icu::Transliterator::createFromRules(
      UNICODE_STRING_SIMPLE("DropAcc"),
      icu::UnicodeString::fromUTF8("::NFD; ::[:Nonspacing Mark:] Remove; ::NFC;"
                                   " ?? > l; ?? > o; ?? > d;"),
      UTRANS_FORWARD, parse_error, status));

  // Supplement the Unicode confusable list by the following mapping.
  //  NOTE: Adding a digit-lookalike? Add it to digit_lookalikes_ above, too.
  //   - {U+00E6 (??), U+04D5 (??)}  => "ae"
  //   - {U+03FC (??), U+048F (??)} => p
  //   - {U+0127 (??), U+043D (??), U+045B (??), U+04A3 (??), U+04A5 (??),
  //      U+04C8 (??), U+04CA (??), U+050B (??), U+0527 (??), U+0529 (??)} => h
  //   - {U+0138 (??), U+03BA (??), U+043A (??), U+049B (??), U+049D (??),
  //      U+049F (??), U+04A1(??), U+04C4 (??), U+051F (??)} => k
  //   - {U+014B (??), U+043F (??), U+0525 (??), U+0E01 (???), U+05D7 (??)} => n
  //   - U+0153 (??) => "ce"
  //     TODO: see https://crbug.com/843352 for further work on
  //     U+0525 and U+0153.
  //   - {U+0167 (??), U+0442 (??), U+04AD (??), U+050F (??), U+4E03 (???),
  //     U+4E05 (???), U+4E06 (???), U+4E01 (???)} => t
  //   - {U+0185 (??), U+044C (??), U+048D (??), U+0432 (??)} => b
  //   - {U+03C9 (??), U+0448 (??), U+0449 (??), U+0E1E (???),
  //      U+0E1F (???), U+0E9E (???), U+0E9F (???)} => w
  //   - {U+043C (??), U+04CE (??)} => m
  //   - {U+0454 (??), U+04BD (??), U+04BF (??), U+1054 (???)} => e
  //   - U+0491 (??) => r
  //   - {U+0493 (??), U+04FB (??)} => f
  //   - {U+04AB (??), U+1004 (???)} => c
  //   - {U+04B1 (??), U+4E2B (???)} => y
  //   - {U+03C7 (??), U+04B3 (??), U+04FD (??), U+04FF (??), U+4E42 (???)} => x
  //   - {U+0503 (??), U+10EB (???)} => d
  //   - {U+050D (??), U+100c (???)} => g
  //   - {U+0D1F (???), U+0E23 (???), U+0EA3 (???), U+0EAE (???)} => s
  //   - U+1042 (???) => j
  //   - {U+0966 (???), U+09E6 (???), U+0A66 (???), U+0AE6 (???), U+0B30 (???),
  //      U+0B66 (???), U+0CE6 (???)} => o,
  //   - {U+09ED (???), U+0A67 (???), U+0AE7 (???)} => q,
  //   - {U+0E1A (???), U+0E9A (???)} => u,
  //   - {U+03B8 (??)} => 0,
  //   - {U+0968 (???), U+09E8 (???), U+0A68 (???), U+0A68 (???), U+0AE8 (???),
  //      U+0ce9 (???), U+0ced (???), U+0577 (??)} => 2,
  //   - {U+0437 (??), U+0499 (??), U+04E1 (??), U+0909 (???), U+0993 (???),
  //      U+0A24 (???), U+0A69 (???), U+0AE9 (???), U+0C69 (???),
  //      U+1012 (???), U+10D5 (???), U+10DE (???)} => 3
  //   - {U+0A6B (???), U+4E29 (???), U+3110 (???)} => 4,
  //   - U+0573 (??) => 6
  //   - {U+09EA (???), U+0A6A (???), U+0b6b (???)} => 8,
  //   - {U+0AED (???), U+0b68 (???), U+0C68 (???)} => 9,
  //   Map a few dashes that ICU doesn't map. These are already blocked by ICU,
  //   but mapping them allows us to detect same skeletons.
  //   - {U+2014 (???), U+4E00 (???), U+2015 (???), U+23EA (???), U+2E3B (???)} => -,
  extra_confusable_mapper_.reset(icu::Transliterator::createFromRules(
      UNICODE_STRING_SIMPLE("ExtraConf"),
      icu::UnicodeString::fromUTF8(
          "[????] > ae; [????] > p; [????????????????????] > h;"
          "[??????????????????] > k; [???????????] > n; ?? > ce;"
          "[????????????????????] > t; [????????] > b;  [??????????????????] > w;"
          "[????] > m; [?????????] > e; ?? > r; [????] > f;"
          "[?????] > c; [?????] > y; [???????????] > x;"
          "[?????]  > d; [?????] > g; [????????????] > s; ??? > j;"
          "[?????????????????????] > o;"
          "[?????????] > q;"
          "[??????] > u;"
          "[??] > 0;"
          "[???????????????????????] > 2;"
          "[?????????????????????????????????] > 3;"
          "[?????????] > 4;"
          "[??] > 6;"
          "[?????????] > 8;"
          "[?????????] > 9;"
          "[???????????????] > \\-;"),
      UTRANS_FORWARD, parse_error, status));
  DCHECK(U_SUCCESS(status))
      << "Spoofchecker initalization failed due to an error: "
      << u_errorName(status);
}

IDNSpoofChecker::~IDNSpoofChecker() {
  uspoof_close(checker_);
}

bool IDNSpoofChecker::SafeToDisplayAsUnicode(
    base::StringPiece16 label,
    base::StringPiece top_level_domain,
    base::StringPiece16 top_level_domain_unicode) {
  UErrorCode status = U_ZERO_ERROR;
  int32_t result =
      uspoof_check(checker_, label.data(),
                   base::checked_cast<int32_t>(label.size()), nullptr, &status);
  // If uspoof_check fails (due to library failure), or if any of the checks
  // fail, treat the IDN as unsafe.
  if (U_FAILURE(status) || (result & USPOOF_ALL_CHECKS))
    return false;

  icu::UnicodeString label_string(FALSE /* isTerminated */, label.data(),
                                  base::checked_cast<int32_t>(label.size()));

  // A punycode label with 'xn--' prefix is not subject to the URL
  // canonicalization and is stored as it is in GURL. If it encodes a deviation
  // character (UTS 46; e.g. U+00DF/sharp-s), it should be still shown in
  // punycode instead of Unicode. Without this check, xn--fu-hia for
  // 'fu<sharp-s>' would be converted to 'fu<sharp-s>' for display because
  // "UTS 46 section 4 Processing step 4" applies validity criteria for
  // non-transitional processing (i.e. do not map deviation characters) to any
  // punycode labels regardless of whether transitional or non-transitional is
  // chosen. On the other hand, 'fu<sharp-s>' typed or copy and pasted
  // as Unicode would be canonicalized to 'fuss' by GURL and is displayed as
  // such. See http://crbug.com/595263 .
  if (deviation_characters_.containsSome(label_string))
    return false;

  // Disallow Icelandic confusables for domains outside Iceland's ccTLD (.is).
  if (label_string.length() > 1 && top_level_domain != "is" &&
      icelandic_characters_.containsSome(label_string))
    return false;

  // Disallow Latin Schwa (U+0259) for domains outside Azerbaijan's ccTLD (.az).
  if (label_string.length() > 1 && top_level_domain != "az" &&
      label_string.indexOf("??") != -1)
    return false;

  // Disallow middle dot (U+00B7) when unsafe.
  if (HasUnsafeMiddleDot(label_string, top_level_domain)) {
    return false;
  }

  // If there's no script mixing, the input is regarded as safe without any
  // extra check unless it falls into one of three categories:
  //   - contains Kana letter exceptions
  //   - the TLD is ASCII and the input is made entirely of Cyrillic letters
  //     that look like Latin letters.
  //   - it has combining diacritic marks.
  // Note that the following combinations of scripts are treated as a 'logical'
  // single script.
  //  - Chinese: Han, Bopomofo, Common
  //  - Japanese: Han, Hiragana, Katakana, Common
  //  - Korean: Hangul, Han, Common
  result &= USPOOF_RESTRICTION_LEVEL_MASK;
  if (result == USPOOF_ASCII)
    return true;
  if (result == USPOOF_SINGLE_SCRIPT_RESTRICTIVE &&
      kana_letters_exceptions_.containsNone(label_string) &&
      combining_diacritics_exceptions_.containsNone(label_string)) {
    // Check Cyrillic confusable only for TLDs where Cyrillic characters are
    // uncommon.
    return IsCyrillicTopLevelDomain(top_level_domain,
                                    top_level_domain_unicode) ||
           !IsMadeOfLatinAlikeCyrillic(label_string);
  }

  // Disallow domains that contain only numbers and number-spoofs.
  if (IsDigitLookalike(label_string))
    return false;

  // Additional checks for |label| with multiple scripts, one of which is Latin.
  // Disallow non-ASCII Latin letters to mix with a non-Latin script.
  // Note that the non-ASCII Latin check should not be applied when the entire
  // label is made of Latin. Checking with lgc_letters set here should be fine
  // because script mixing of LGC is already rejected.
  if (non_ascii_latin_letters_.containsSome(label_string) &&
      !lgc_letters_n_ascii_.containsAll(label_string))
    return false;

  icu::RegexMatcher* dangerous_pattern =
      reinterpret_cast<icu::RegexMatcher*>(DangerousPatternTLS().Get());
  if (!dangerous_pattern) {
    // The parentheses in the below strings belong to the raw string sequence
    // R"(...)". They are NOT part of the regular expression. Each sub
    // regex is OR'ed with the | operator.
    dangerous_pattern = new icu::RegexMatcher(
        icu::UnicodeString(
            // Disallow the following as they may be mistaken for slashes when
            // they're surrounded by non-Japanese scripts (i.e. has non-Katakana
            // Hiragana or Han scripts on both sides):
            // "???" (Katakana no, U+30ce), "???" (Katakana so, U+30bd),
            // "???" (Katakana zo, U+30be), "???" (Katakana n, U+30f3),
            // "???" (CJK unified ideograph, U+4E36),
            // "???" (CJK unified ideograph, U+4E40),
            // "???" (CJK unified ideograph, U+4E41),
            // "???" (CJK unified ideograph, U+4E3F).
            // If {no, so, zo, n} next to a
            // non-Japanese script on either side is disallowed, legitimate
            // cases like '{vitamin in Katakana}b6' are blocked. Note that
            // trying to block those characters when used alone as a label is
            // futile because those cases would not reach here. Also disallow
            // what used to be blocked by mixed-script-confusable (MSC)
            // detection. ICU 58 does not detect MSC any more for a single input
            // string. See http://bugs.icu-project.org/trac/ticket/12823 .
            // TODO(jshin): adjust the pattern once the above ICU bug is fixed.
            R"([^\p{scx=kana}\p{scx=hira}\p{scx=hani}])"
            R"([\u30ce\u30f3\u30bd\u30be\u4e36\u4e40\u4e41\u4e3f])"
            R"([^\p{scx=kana}\p{scx=hira}\p{scx=hani}]|)"

            // Disallow U+30FD (Katakana iteration mark) and U+30FE (Katakana
            // voiced iteration mark) unless they're preceded by a Katakana.
            R"([^\p{scx=kana}][\u30fd\u30fe]|^[\u30fd\u30fe]|)"

            // Disallow three Hiragana letters (U+307[8-A]) or Katakana letters
            // (U+30D[8-A]) that look exactly like each other when they're used
            // in a label otherwise entirely in Katakana or Hiragana.
            R"(^[\p{scx=kana}]+[\u3078-\u307a][\p{scx=kana}]+$|)"
            R"(^[\p{scx=hira}]+[\u30d8-\u30da][\p{scx=hira}]+$|)"

            // Disallow U+30FB (Katakana Middle Dot) and U+30FC (Hiragana-
            // Katakana Prolonged Sound) used out-of-context.
            R"([^\p{scx=kana}\p{scx=hira}]\u30fc|^\u30fc|)"
            R"([a-z]\u30fb|\u30fb[a-z]|)"

            // Disallow these CJK ideographs if they are next to non-CJK
            // characters. These characters can be used to spoof Latin
            // characters or punctuation marks:
            // U+4E00 (???), U+3127 (???), U+4E28 (???), U+4E5B (???), U+4E03 (???),
            // U+4E05 (???), U+5341 (???), U+3007 (???), U+3112 (???), U+311A (???),
            // U+311F (???), U+3128 (???), U+3129 (???), U+3108 (???), U+31BA (???),
            // U+31B3 (???), U+5DE5 (???), U+31B2 (???), U+8BA0 (???), U+4E01 (???)
            // These characters are already blocked:
            // U+2F00 (???) (normalized to U+4E00), U+3192 (???), U+2F02 (???),
            // U+2F17 (???) and U+3038 (???) (both normalized to U+5341 (???)).
            // Check if there is non-{Hiragana, Katagana, Han, Bopomofo} on the
            // left.
            R"([^\p{scx=kana}\p{scx=hira}\p{scx=hani}\p{scx=bopo}])"
            R"([\u4e00\u3127\u4e28\u4e5b\u4e03\u4e05\u5341\u3007\u3112)"
            R"(\u311a\u311f\u3128\u3129\u3108\u31ba\u31b3\u5dE5)"
            R"(\u31b2\u8ba0\u4e01]|)"
            // Check if there is non-{Hiragana, Katagana, Han, Bopomofo} on the
            // right.
            R"([\u4e00\u3127\u4e28\u4e5b\u4e03\u4e05\u5341\u3007\u3112)"
            R"(\u311a\u311f\u3128\u3129\u3108\u31ba\u31b3\u5de5)"
            R"(\u31b2\u8ba0\u4e01])"
            R"([^\p{scx=kana}\p{scx=hira}\p{scx=hani}\p{scx=bopo}]|)"

            // Disallow combining diacritical mark (U+0300-U+0339) after a
            // non-LGC character. Other combining diacritical marks are not in
            // the allowed character set.
            R"([^\p{scx=latn}\p{scx=grek}\p{scx=cyrl}][\u0300-\u0339]|)"

            // Disallow dotless i (U+0131) followed by a combining mark.
            R"(\u0131[\u0300-\u0339]|)"

            // Disallow combining Kana voiced sound marks.
            R"(\u3099|\u309A|)"

            // Disallow U+0307 (dot above) after 'i', 'j', 'l' or dotless i
            // (U+0131). Dotless j (U+0237) is not in the allowed set to begin
            // with.
            R"([ijl]\u0307)",
            -1, US_INV),
        0, status);
    DangerousPatternTLS().Set(dangerous_pattern);
  }
  dangerous_pattern->reset(label_string);
  return !dangerous_pattern->find();
}

TopDomainEntry IDNSpoofChecker::GetSimilarTopDomain(
    base::StringPiece16 hostname) {
  DCHECK(!hostname.empty());
  for (const std::string& skeleton : GetSkeletons(hostname)) {
    DCHECK(!skeleton.empty());
    TopDomainEntry matching_top_domain = LookupSkeletonInTopDomains(skeleton);
    if (!matching_top_domain.domain.empty()) {
      return matching_top_domain;
    }
  }
  return TopDomainEntry();
}

Skeletons IDNSpoofChecker::GetSkeletons(base::StringPiece16 hostname) {
  Skeletons skeletons;
  size_t hostname_length = hostname.length() - (hostname.back() == '.' ? 1 : 0);
  icu::UnicodeString host(FALSE, hostname.data(), hostname_length);
  // If input has any characters outside Latin-Greek-Cyrillic and [0-9._-],
  // there is no point in getting rid of diacritics because combining marks
  // attached to non-LGC characters are already blocked.
  if (lgc_letters_n_ascii_.span(host, 0, USET_SPAN_CONTAINED) == host.length())
    diacritic_remover_->transliterate(host);
  extra_confusable_mapper_->transliterate(host);

  UErrorCode status = U_ZERO_ERROR;
  icu::UnicodeString ustr_skeleton;

  // Map U+04CF (??) to lowercase L in addition to what uspoof_getSkeleton does
  // (mapping it to lowercase I).
  int32_t u04cf_pos;
  if ((u04cf_pos = host.indexOf(0x4CF)) != -1) {
    icu::UnicodeString host_alt(host);
    size_t length = host_alt.length();
    char16_t* buffer = host_alt.getBuffer(-1);
    for (char16_t* uc = buffer + u04cf_pos; uc < buffer + length; ++uc) {
      if (*uc == 0x4CF)
        *uc = 0x6C;  // Lowercase L
    }
    host_alt.releaseBuffer(length);
    uspoof_getSkeletonUnicodeString(checker_, 0, host_alt, ustr_skeleton,
                                    &status);
    if (U_SUCCESS(status)) {
      std::string skeleton;
      ustr_skeleton.toUTF8String(skeleton);
      skeletons.insert(skeleton);
    }
  }

  uspoof_getSkeletonUnicodeString(checker_, 0, host, ustr_skeleton, &status);
  if (U_SUCCESS(status)) {
    std::string skeleton;
    ustr_skeleton.toUTF8String(skeleton);
    skeletons.insert(skeleton);
  }
  return skeletons;
}

TopDomainEntry IDNSpoofChecker::LookupSkeletonInTopDomains(
    const std::string& skeleton) {
  DCHECK(!skeleton.empty());
  // There are no other guarantees about a skeleton string such as not including
  // a dot. Skeleton of certain characters are dots (e.g. "??" (U+06F0)).
  TopDomainPreloadDecoder preload_decoder(
      g_trie_params.huffman_tree, g_trie_params.huffman_tree_size,
      g_trie_params.trie, g_trie_params.trie_bits,
      g_trie_params.trie_root_position);
  auto labels = base::SplitStringPiece(skeleton, ".", base::KEEP_WHITESPACE,
                                       base::SPLIT_WANT_ALL);

  if (labels.size() > kNumberOfLabelsToCheck) {
    labels.erase(labels.begin(),
                 labels.begin() + labels.size() - kNumberOfLabelsToCheck);
  }

  while (labels.size() > 1) {
    std::string partial_skeleton = base::JoinString(labels, ".");
    bool match = false;
    bool decoded = preload_decoder.Decode(partial_skeleton, &match);
    DCHECK(decoded);
    if (!decoded)
      return TopDomainEntry();

    if (match)
      return preload_decoder.matching_top_domain();

    labels.erase(labels.begin());
  }
  return TopDomainEntry();
}

void IDNSpoofChecker::SetAllowedUnicodeSet(UErrorCode* status) {
  if (U_FAILURE(*status))
    return;

  // The recommended set is a set of characters for identifiers in a
  // security-sensitive environment taken from UTR 39
  // (http://unicode.org/reports/tr39/) and
  // http://www.unicode.org/Public/security/latest/xidmodifications.txt .
  // The inclusion set comes from "Candidate Characters for Inclusion
  // in idenfiers" of UTR 31 (http://www.unicode.org/reports/tr31). The list
  // may change over the time and will be updated whenever the version of ICU
  // used in Chromium is updated.
  const icu::UnicodeSet* recommended_set =
      uspoof_getRecommendedUnicodeSet(status);
  icu::UnicodeSet allowed_set;
  allowed_set.addAll(*recommended_set);
  const icu::UnicodeSet* inclusion_set = uspoof_getInclusionUnicodeSet(status);
  allowed_set.addAll(*inclusion_set);

  // The sections below refer to Mozilla's IDN blacklist:
  // http://kb.mozillazine.org/Network.IDN.blacklist_chars
  //
  // U+0338 (Combining Long Solidus Overlay) is included in the recommended set,
  // but is blacklisted by Mozilla. It is dropped because it can look like a
  // slash when rendered with a broken font.
  allowed_set.remove(0x338u);
  // U+05F4 (Hebrew Punctuation Gershayim) is in the inclusion set, but is
  // blacklisted by Mozilla. We keep it, even though it can look like a double
  // quotation mark. Using it in Hebrew should be safe. When used with a
  // non-Hebrew script, it'd be filtered by other checks in place.

  // The following 5 characters are disallowed because they're in NV8 (invalid
  // in IDNA 2008).
  allowed_set.remove(0x58au);  // Armenian Hyphen
  // U+2010 (Hyphen) is in the inclusion set, but we drop it because it can be
  // confused with an ASCII U+002D (Hyphen-Minus).
  allowed_set.remove(0x2010u);
  // U+2019 is hard to notice when sitting next to a regular character.
  allowed_set.remove(0x2019u);  // Right Single Quotation Mark
  // U+2027 (Hyphenation Point) is in the inclusion set, but is blacklisted by
  // Mozilla. It is dropped, as it can be confused with U+30FB (Katakana Middle
  // Dot).
  allowed_set.remove(0x2027u);
  allowed_set.remove(0x30a0u);  // Katakana-Hiragana Double Hyphen

  // Block {Single,double}-quotation-mark look-alikes.
  allowed_set.remove(0x2bbu);  // Modifier Letter Turned Comma
  allowed_set.remove(0x2bcu);  // Modifier Letter Apostrophe

  // Block modifier letter voicing.
  allowed_set.remove(0x2ecu);

  // Block historic character Latin Kra (also blocked by Mozilla).
  allowed_set.remove(0x0138);

  // No need to block U+144A (Canadian Syllabics West-Cree P) separately
  // because it's blocked from mixing with other scripts including Latin.

#if defined(OS_MACOSX)
  // The following characters are reported as present in the default macOS
  // system UI font, but they render as blank. Remove them from the allowed
  // set to prevent spoofing until the font issue is resolved.

  // Arabic letter KASHMIRI YEH. Not used in Arabic and Persian.
  allowed_set.remove(0x0620u);

  // Tibetan characters used for transliteration of ancient texts:
  allowed_set.remove(0x0F8Cu);
  allowed_set.remove(0x0F8Du);
  allowed_set.remove(0x0F8Eu);
  allowed_set.remove(0x0F8Fu);
#endif

  // Disallow extremely rarely used LGC character blocks.
  // Cyllic Ext A is not in the allowed set. Neither are Latin Ext-{C,E}.
  allowed_set.remove(0x01CDu, 0x01DCu);  // Latin Ext B; Pinyin
  allowed_set.remove(0x1C80u, 0x1C8Fu);  // Cyrillic Extended-C
  allowed_set.remove(0x1E00u, 0x1E9Bu);  // Latin Extended Additional
  allowed_set.remove(0x1F00u, 0x1FFFu);  // Greek Extended
  allowed_set.remove(0xA640u, 0xA69Fu);  // Cyrillic Extended-B
  allowed_set.remove(0xA720u, 0xA7FFu);  // Latin Extended-D

  uspoof_setAllowedUnicodeSet(checker_, &allowed_set, status);
}

bool IDNSpoofChecker::IsMadeOfLatinAlikeCyrillic(
    const icu::UnicodeString& label) {
  // Collect all the Cyrillic letters in |label_string| and see if they're
  // a subset of |cyrillic_letters_latin_alike_|.
  // A shortcut of defining cyrillic_letters_latin_alike_ to include [0-9] and
  // [_-] and checking if the set contains all letters of |label|
  // would work in most cases, but not if a label has non-letters outside
  // ASCII.
  icu::UnicodeSet cyrillic_in_label;
  icu::StringCharacterIterator it(label);
  for (it.setToStart(); it.hasNext();) {
    const UChar32 c = it.next32PostInc();
    if (cyrillic_letters_.contains(c))
      cyrillic_in_label.add(c);
  }
  return !cyrillic_in_label.isEmpty() &&
         cyrillic_letters_latin_alike_.containsAll(cyrillic_in_label);
}

bool IDNSpoofChecker::IsDigitLookalike(const icu::UnicodeString& label) {
  bool has_lookalike_char = false;
  icu::StringCharacterIterator it(label);
  for (it.setToStart(); it.hasNext();) {
    const UChar32 c = it.next32PostInc();
    if (digits_.contains(c)) {
      continue;
    }
    if (digit_lookalikes_.contains(c)) {
      has_lookalike_char = true;
      continue;
    }
    return false;
  }
  return has_lookalike_char;
}

bool IDNSpoofChecker::IsCyrillicTopLevelDomain(
    base::StringPiece tld,
    base::StringPiece16 tld_unicode) const {
  icu::UnicodeString tld_string(
      FALSE /* isTerminated */, tld_unicode.data(),
      base::checked_cast<int32_t>(tld_unicode.size()));
  if (cyrillic_letters_.containsSome(tld_string)) {
    return true;
  }
  // These ASCII TLDs contain a large number of domains with Cyrillic
  // characters.
  return tld == "bg" || tld == "by" || tld == "kz" || tld == "pyc" ||
         tld == "ru" || tld == "su" || tld == "ua" || tld == "uz";
}

// static
void IDNSpoofChecker::SetTrieParamsForTesting(
    const HuffmanTrieParams& trie_params) {
  g_trie_params = trie_params;
}

// static
void IDNSpoofChecker::RestoreTrieParamsForTesting() {
  g_trie_params = HuffmanTrieParams{
      kTopDomainsHuffmanTree, sizeof(kTopDomainsHuffmanTree), kTopDomainsTrie,
      kTopDomainsTrieBits, kTopDomainsRootPosition};
}

}  // namespace url_formatter
