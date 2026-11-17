#pragma once

#include <cstddef>
#include <intsh>

#include "technology.hpp"

using namespace br0::intsh;

namespace save {

    /**
     * @brief Why a call did not do its job; 0 (::ok) means it did.
     *
     * UNSCOPED AND u8-BASED, DELIBERATELY: `if (!response.err)` reads as the
     * one-line success test, exactly as a bare u8 would, while `case
     * save::no_device:` still names itself in a switch. `enum class` would have
     * cost the first for the sake of the second, and both spellings compile to
     * the same byte -- a u8-based enum is one byte, and a Response carrying one
     * is the same size as a Response carrying a plain u8.
     *
     * LIBRARY-OWNED, NOT A PLATFORM'S OWN CODES. Each backend translates into
     * this: libogc's CARD_ERROR_NOCARD is -3, which would arrive in a u8 as
     * 253, and a game switching on that is writing GameCube-specific magic
     * numbers -- the opposite of being told what went wrong in terms it can
     * act on. The platform's own code stays in its backend, for a debugger.
     *
     * NOTHING FOUND IS NOT AN ERROR. A search of a brand-new memory card
     * legitimately finds nothing, and that is the Get-then-Create flow working,
     * not failing -- so it reports ::ok with a null ::Response::path, and a
     * game that treats `err != 0` as a fault never shows "save error" to a
     * player whose card is merely blank. ::not_found means only "you named a
     * file and it is not there".
     */
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

    AI Response Save(const u8* src, std::size_t size);
    AI Response Load(u8* tar, std::size_t size);
    AI Response Load(u8* tar, std::size_t size, const Response* response);
    AI Response CreateSave(const u8* src, std::size_t size, const Response* response);
    AI Response GetSaveLocation();
}