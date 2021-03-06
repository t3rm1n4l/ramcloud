/* Copyright (c) 2011 Facebook
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"
#include "StringUtil.h"

namespace RAMCloud {

using namespace StringUtil; // NOLINT

TEST(StringUtilTest, startsWith) {
    EXPECT_TRUE(startsWith("foo", "foo"));
    EXPECT_TRUE(startsWith("foo", "fo"));
    EXPECT_TRUE(startsWith("foo", ""));
    EXPECT_TRUE(startsWith("", ""));
    EXPECT_FALSE(startsWith("f", "foo"));
}

TEST(StringUtilTest, endsWith) {
    EXPECT_TRUE(endsWith("foo", "foo"));
    EXPECT_TRUE(endsWith("foo", "oo"));
    EXPECT_TRUE(endsWith("foo", ""));
    EXPECT_TRUE(endsWith("", ""));
    EXPECT_FALSE(endsWith("o", "foo"));
}

}  // namespace RAMCloud
