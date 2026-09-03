/*
 * Which site a path belongs to, and which file of it -- the two decisions the static web
 * server makes before it answers anything.
 *
 * They are tested here rather than through the server because that is where they can be:
 * both are pure functions over a table, and the table is normally written by the build out of
 * `publish/sites.json`. A test that used the generated one would assert what somebody's last
 * `bun run publish` happened to produce; these build their own.
 *
 * What is *not* asserted here is the rest of the behaviour -- the ETag, the cache header, the
 * Accept rule that decides between the app and ROUTE_NOT_IMPLEMENTED. That is HTTP, it has to
 * be identical on both backends, and `tests/integration/` is where both are driven over a
 * socket. The reference path holds the same rules in
 * `packages/backend/src/server/staticRoutes.test.ts`.
 *
 * C++ because googletest is; see the note at the top of service-core/tests/test_cache.cpp.
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "backend/static_sites.h"
}

namespace
{

backend_static_file file_of(const char *path)
{
    backend_static_file file{};

    file.path = path;
    file.path_length = std::strlen(path);
    file.bytes = "x";
    file.length = 1;
    file.content_type = "text/plain; charset=utf-8";
    file.etag = "\"1\"";
    file.immutable = 0;
    return file;
}

/* Sorted by path, which is what the generated table is and what the search assumes. */
const backend_static_file kFiles[] = {
    file_of("assets/index-abc.css"),
    file_of("assets/index-abc.js"),
    file_of("favicon.png"),
    file_of("img/brand/logo.png"),
    file_of("index.html"),
    file_of("locales/de/messages.json"),
    file_of("locales/en/messages.json"),
};

const backend_static_site kSites[] = {
    {"frontend", "", 0, kFiles, sizeof(kFiles) / sizeof(kFiles[0]), &kFiles[4]},
    {"admin", "/admin", 6, kFiles, sizeof(kFiles) / sizeof(kFiles[0]), &kFiles[4]},
};

const backend_static_site *site_for(const char *path, size_t count = 2)
{
    return backend_static_site_for(kSites, count, path, std::strlen(path));
}

const backend_static_file *find(const backend_static_site *site, const char *path)
{
    return backend_static_file_find(site, path, std::strlen(path));
}

TEST(StaticSiteFor, ASiteAtTheRootClaimsEverything)
{
    EXPECT_STREQ(site_for("/", 1)->name, "frontend");
    EXPECT_STREQ(site_for("/login", 1)->name, "frontend");
    EXPECT_STREQ(site_for("/user/create", 1)->name, "frontend");
}

TEST(StaticSiteFor, TheLongerBasePathWins)
{
    EXPECT_STREQ(site_for("/admin")->name, "admin");
    EXPECT_STREQ(site_for("/admin/")->name, "admin");
    EXPECT_STREQ(site_for("/admin/settings")->name, "admin");
    EXPECT_STREQ(site_for("/login")->name, "frontend");
}

TEST(StaticSiteFor, ASubPathIsASegmentAndNotAPrefix)
{
    /* "/administration" starts with "/admin" and is not below it. It belongs to whatever
     * claims the root, which here is the frontend. */
    EXPECT_STREQ(site_for("/administration")->name, "frontend");
    EXPECT_STREQ(site_for("/adminx/y")->name, "frontend");
}

TEST(StaticSiteFor, NoSiteClaimsAnythingWhenThereAreNone)
{
    /* What a build without pages carries, and what the server then answers for every path:
     * nothing here, so the default route falls through to ROUTE_NOT_IMPLEMENTED. */
    EXPECT_EQ(backend_static_site_for(kSites, 0, "/", 1), nullptr);
}

TEST(StaticSiteFor, ASubPathSiteAloneClaimsOnlyItsOwn)
{
    EXPECT_EQ(backend_static_site_for(&kSites[1], 1, "/login", 6), nullptr);
    EXPECT_STREQ(backend_static_site_for(&kSites[1], 1, "/admin/x", 8)->name, "admin");
}

TEST(StaticFileFind, FindsEveryFileInTheTable)
{
    const backend_static_site *site = site_for("/", 1);

    for (const backend_static_file &file : kFiles) {
        const backend_static_file *found = find(site, file.path);
        ASSERT_NE(found, nullptr) << file.path;
        EXPECT_STREQ(found->path, file.path);
    }
}

TEST(StaticFileFind, DoesNotFindWhatIsNotThere)
{
    const backend_static_site *site = site_for("/", 1);

    EXPECT_EQ(find(site, "assets/gone.js"), nullptr);
    EXPECT_EQ(find(site, "zzz"), nullptr);
    EXPECT_EQ(find(site, "a"), nullptr);
    EXPECT_EQ(find(site, ""), nullptr);
}

TEST(StaticFileFind, APrefixIsNotAMatch)
{
    const backend_static_site *site = site_for("/", 1);

    /* Both directions: a path that is a prefix of an entry, and an entry that is a prefix of
     * the path. A comparison that stopped at the shorter length would answer both. */
    EXPECT_EQ(find(site, "assets/index-abc.j"), nullptr);
    EXPECT_EQ(find(site, "assets/index-abc.jsx"), nullptr);
    EXPECT_EQ(find(site, "index"), nullptr);
}

TEST(StaticFileFind, ATraversalIsSimplyNotAKey)
{
    const backend_static_site *site = site_for("/", 1);

    /* Nothing rejects these; there is nothing to reject. The table holds what the build put in
     * it, and a path that is not one of those is not found -- which is the whole of the
     * defence, and why there is no path canonicalisation anywhere in this server. */
    EXPECT_EQ(find(site, "../../etc/passwd"), nullptr);
    EXPECT_EQ(find(site, "assets/../../../etc/passwd"), nullptr);
}

TEST(StaticFileFind, AnEmptySiteHasNoFiles)
{
    backend_static_site empty = {"empty", "", 0, nullptr, 0, nullptr};

    EXPECT_EQ(find(&empty, "index.html"), nullptr);
}

} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
