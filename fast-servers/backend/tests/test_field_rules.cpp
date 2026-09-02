/*
 * The field rules of packages/shared/src/schemas, as the route applies them.
 *
 * These are the one place this implementation reproduces somebody else's library rather than
 * somebody else's behaviour: the reference route declares a valibot schema and gets the
 * refusals, the messages and the lengths for free. What is asserted here is the two places that
 * reproduction is easy to get subtly wrong, and both of them are about JavaScript rather than
 * about email addresses:
 *
 *   length   valibot counts UTF-16 code units. A German name is full of characters that are one
 *            unit and two bytes, so a byte count would refuse names the reference path accepts.
 *   trim     `String.prototype.trim` removes more than C would call whitespace -- U+00A0 and
 *            U+FEFF among it, both of which arrive in a value pasted out of a web page.
 *
 * The expectations were taken from valibot 1.4.2 itself rather than read off its source.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <string>

extern "C" {
#include "field_rules.h"
}

namespace
{

size_t units(const std::string &text) { return bk_utf16_length(text.c_str(), text.size()); }

std::string trimmed(const std::string &text)
{
    size_t begin = 0;
    size_t length = 0;

    bk_trim(text.c_str(), text.size(), &begin, &length);
    return text.substr(begin, length);
}

bool is_email(const std::string &text) { return bk_is_email(text.c_str(), text.size()) != 0; }

TEST(Utf16Length, CountsCodeUnitsAndNotBytes)
{
    EXPECT_EQ(units("Max"), 3u);
    /* Two bytes, one unit -- and the reason this function exists. */
    EXPECT_EQ(units("Müller"), 6u);
    /* Three bytes, one unit. */
    EXPECT_EQ(units("東京"), 2u);
    /* Four bytes, and a surrogate pair in a JavaScript string, so two. */
    EXPECT_EQ(units("😀"), 2u);
    EXPECT_EQ(units(""), 0u);
}

TEST(Trim, RemovesWhatJavaScriptRemoves)
{
    EXPECT_EQ(trimmed("  Max  "), "Max");
    EXPECT_EQ(trimmed("\t\r\nMax\n"), "Max");
    /* U+00A0, the non-breaking space a web page hands over. */
    EXPECT_EQ(trimmed("\xc2\xa0Max\xc2\xa0"), "Max");
    /* U+FEFF, the byte order mark, which JavaScript also trims. */
    EXPECT_EQ(trimmed("\xef\xbb\xbfMax"), "Max");
    /* Interior whitespace is not touched. */
    EXPECT_EQ(trimmed(" Max Mustermann "), "Max Mustermann");
    EXPECT_EQ(trimmed("   "), "");
}

TEST(Email, AcceptsWhatValibotAccepts)
{
    EXPECT_TRUE(is_email("a@b.co"));
    EXPECT_TRUE(is_email("Max.Mustermann+tag@sub-domain.example.org"));
    EXPECT_TRUE(is_email("A@B.CO"));
    EXPECT_TRUE(is_email("a_b@x1.io"));
    EXPECT_TRUE(is_email("a@1.io"));
    EXPECT_TRUE(is_email("a+b@c.d.ef"));
    EXPECT_TRUE(is_email("a@b.c-d.com"));
}

TEST(Email, RefusesWhatValibotRefuses)
{
    EXPECT_FALSE(is_email(""));
    EXPECT_FALSE(is_email("nope"));
    /* One letter is not a top level domain to this rule. */
    EXPECT_FALSE(is_email("a@b.c"));
    /* The last label has to be letters. */
    EXPECT_FALSE(is_email("a@b.co1"));
    EXPECT_FALSE(is_email("a@-b.co"));
    EXPECT_FALSE(is_email("a@b-.co"));
    EXPECT_FALSE(is_email("a@b..co"));
    EXPECT_FALSE(is_email("a@b"));
    EXPECT_FALSE(is_email(".a@b.co"));
    EXPECT_FALSE(is_email("a.@b.co"));
    /* `\w` under the `u` flag is ASCII, so an address with an umlaut in it is not one here --
     * which is the reference path's rule and not an opinion of this one. */
    EXPECT_FALSE(is_email("müller@example.com"));
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
