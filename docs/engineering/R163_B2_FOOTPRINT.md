# r163 B2 — footprint trims: what is already taken, what was skipped, and what nobody may claim yet

**Status: no firmware change in this item.** Every candidate below was checked against the
tree first; two are already implemented and the other two have no provably-safe target.
Nothing here was measured — this tree cannot produce a firmware image (the ESP build is out
of scope for this wave), so **every saving below is UNMEASURED**. A final build should
confirm or correct the numbers before any of them is quoted in release notes.

## 1. wolfSSL debug trace — ALREADY COMPILED OUT (expected ~39 KB, UNMEASURED)

`scripts/patch_wolfssl.py` appends the `#undef DEBUG_WOLFSSL` override to Arduino-wolfSSL's
`user_settings.h` (see the "C6" block, lines 28-48). It is wired into **both** build paths:

- PlatformIO: `platformio.ini:137` — `pre:scripts/patch_wolfssl.py`.
- ESP-IDF: `idf/components/crosspoint_app/CMakeLists.txt` runs the same script at configure
  time (this build never reads `platformio.ini`).

Call-graph check that the second, intended effect (`NO_ERROR_STRINGS`) is safe — the claim is
that this firmware never formats a wolfSSL error, it only reads integer codes:

```
$ rg -n "ERR_error_string|wolfSSL_ERR_error_string" src lib freeink-sdk --glob '!**/wolfssl/**'
(no matches)
$ rg -n "wolfSSL_get_error" freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp
135:    const int err = wolfSSL_get_error(ssl, ret);
204:  const int err = wolfSSL_get_error(ssl, n);
```

Both call sites consume the `int` and map it through the file's own table (line 50 comment:
"Only the wolfSSL_get_error() codes"). Nothing reads the error-string table, so removing it
cannot change a handshake outcome, a certificate decision or a message the user sees.

**Saving: UNMEASURED (~39 KB of flash is the patch note's own estimate).**

## 2. Web UI — ALREADY MINIFIED AND GZIPPED AT BUILD TIME (expected ~23 KB, UNMEASURED)

`scripts/build_html.py` minifies each page and then gzips it at `compresslevel=9`, emitting
one blob per page — not a raw copy plus a compressed copy:

- `src/network/html/HomePageHtml.generated.h`: `HomePageHtml[]` (gzip bytes) plus
  `HomePageHtmlCompressedSize = 2103` / `HomePageHtmlOriginalSize = 6074`.
- `CrossPointWebServer::sendStaticContent()` sends exactly that blob with
  `Content-Encoding: gzip` via `send_P()` (`src/network/CrossPointWebServer.cpp:481-495`),
  i.e. streamed from PROGMEM with **no heap copy** of the uncompressed page.

So there is no second, unminified copy to remove and no "loaded on demand" split to make:
the firmware ships the minified, compressed page once and the browser decompresses it.

**Saving: UNMEASURED (~23 KB is the candidate's own estimate; whatever remains would be a
further re-minification, which is a web-UI change, not a firmware trim).**

## 3. ~5 KB of heap during the web server — SKIPPED, no such allocation found

Searched for fixed-size buffers and reserves in the web-server path:

```
$ rg -n "5120|5 \* 1024" src/network src/activities/network        # no matches
$ rg -n "UPLOAD_BUFFER_SIZE|BUFFER_SIZE =" src/network/CrossPointWebServer.{h,cpp}
CrossPointWebServer.h:45:  static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
CrossPointWebServer.h:137: static constexpr size_t BUFFER_SIZE = 4096;
```

Both 4 KB buffers are **transient and already released**: the upload buffer is a
`makeUniqueNoThrow<uint8_t[]>` allocated when an upload starts and `reset()` on every exit
path (`CrossPointWebServer.cpp:891-935`), and the font-upload buffer is the same pattern
(`:2955`). Shrinking either would change upload throughput; freeing them earlier would mean
freeing a buffer that is still in use. There is nothing here that can be removed *without*
changing behaviour, so nothing was removed. A 5 KB figure that does not correspond to an
allocation in this tree cannot be honoured.

## 4. The bookmark-cache reserve — SKIPPED, the only reserves are exact-size

```
$ rg -n "reserve\(" src lib | rg -i "bookmark"
src/util/BookmarkFile.cpp:22:               bookmarks.reserve(arr.size());
src/activities/reader/EpubReaderBookmarksActivity.cpp:51:  bookmarkSubtitles.reserve(bookmarks.size());
src/activities/reader/EpubReaderBookmarksActivity.cpp:52:  bookmarkRowItems.reserve(bookmarks.size());
```

All three reserve **exactly what they are about to fill** (`arr.size()` / `bookmarks.size()`),
so they are not speculative caches: dropping them makes the vector reallocate through its
growth ladder while it copies entries that carry `std::string`s — more peak heap and more
fragmentation, not less. There is no separate bookmark cache holding a standing reserve in
this tree.

## What a build must confirm

1. Flash delta for item 1: compare the trace-on and trace-off images (the patch note's
   ~39 KB) — the only way to make that number real.
2. If item 2 is to be pursued at all, the win is a re-minification of
   `src/network/html/*.html`, measured as the delta of the generated headers' byte counts,
   and it is a web-UI change, not a firmware memory change.
3. Re-run the two `rg` searches in items 3 and 4 before quoting "5 KB" or "bookmark cache"
   anywhere: neither exists in this tree, and a saving that cannot be pointed at an
   allocation should not appear in a release note.
