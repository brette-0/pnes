#pragma once

#include <cstddef>
#include <intsh>

#ifdef TARGET_GC
// card_stat, which is what ::save::GetMetaData / ::save::SetMetaData move. They
// take `void*` rather than naming it, so that this header declares the same two
// functions on every target without a non-GameCube build having to find
// ogc/card.h -- but a GameCube application has to be able to DECLARE one, so
// the type comes in here rather than leaving every call site to include it.
#include <ogc/card.h>
#endif

#include "technology.hpp"

using namespace br0::intsh;

namespace save {

#ifdef TARGET_GC
#define OGC_COMMENT(name) char name [32]
#define OGC_ICON_CI(name) u8 name[1536]
#define OGC_BANNER_CI(name) u8 name[3584]
#endif

    enum eError : u8 {
        found,
        missing,
        no_device,
#ifdef TARGET_GC
        busy,
        exists,
        corrupt,
        full,
        too_large,
        denied,
        bad_path,
        failed,
#endif
    };

    /**
     * @brief The outcome of a ticked save operation.
     *
     * ::done AND ::err ANSWER DIFFERENT QUESTIONS, and conflating them is a
     * hang rather than a wrong answer: ::done says the operation is over, ::err
     * says whether it worked. A call that has not finished reports neither, so
     * `while (!response.done)` is the loop and `if (!response.err)` is the
     * test. Both exist on every target, including the NES, so that a game
     * writes one flow with no `#ifdef` in it -- they cost one byte each and
     * fold away entirely where a backend answers with constants.
     */
    struct Response {
        bool done;
        eError err;
    #ifndef TARGET_NES
        /**
         * @brief Where the save is, or nullptr when there is none.
         *
         * Borrowed from the backend and valid only until the next
         * ::GetSaveLocation call -- copy it to keep it. NOT PRESENT ON NES,
         * where a battery cart's save has no name and no device to be on.
         */
        const char* path;
#endif
    };

    /*
     * NO ::AI ON THESE, AND THAT IS LOAD-BEARING.
     *
     * Every one of them is DEFINED OUT OF LINE, in whichever src/<platform>/
     * save.cpp the build selected. `always_inline` on a declaration whose body
     * the compiler cannot see is not a hint -- GCC REJECTS IT OUTRIGHT:
     *
     *   error: inlining failed in call to 'always_inline'
     *          'save::Save(const u8*, std::size_t)': function body not available
     *
     * ...which breaks every call site in a build without LTO to make the body
     * visible. That is every Debug config (CMakeLists.txt enables IPO for
     * Release/MinSizeRel/RelWithDebInfo only) and the whole PS3 target, which
     * disables it deliberately. Clang merely ignores the attribute, so the
     * problem is invisible on the NES, PS3 and desktop and fatal on the
     * devkitPPC / devkitARM / devkitA64 / PSP / PS2 toolchains.
     *
     * NOTHING IS LOST BY REMOVING IT. On the NES, where these fold to a
     * constant and the fold actually matters, llvm-mos compiles to LTO bitcode
     * BY DEFAULT -- no -flto anywhere in the flags -- and the NES link hands
     * every demo .cpp to one whole-program invocation. The backend's body is
     * therefore visible at link time either way, and a function whose whole
     * body is `return { true, no_device }` is inlined on its merits at -Os
     * without being ordered to be. A measured `while (!...done)` loop around
     * one of these compiles to nothing at all.
     *
     * The convention this follows is video.hpp's: ::video::Is50Hz() is
     * `AI inline` because its body IS in that header, while
     * ::video::DetectRefresh() is declared plain because its body is not.
     * ::AI belongs on a definition, not on a promise (technology.hpp).
     */
    Response SetMetaData(const void* src);
    Response GetMetaData(void* tar);
    Response Save(const u8* src, std::size_t size);
    Response Load(u8* tar, std::size_t size);
    Response Load(u8* tar, std::size_t size, const Response* response);
    Response CreateSave(const u8* src, std::size_t size, const Response* response);
    Response GetSaveLocation();
}