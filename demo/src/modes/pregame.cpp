#include "pregame.hpp"
#include "../save.hpp"
#include <pnes/save.hpp>

#include <intsh>

#include "pnes/input.hpp"
#include "pnes/logger.hpp"

using namespace br0::intsh;

static constexpr void WaitBody() {
    video::WaitForPresent();
}

static void LoadSave();
static bool ValidateSave();
static void AskCreateSave();
static void PrintContext(const u8* infoText);
static eOptions ChoiceWheel(const eOptions* pOptions, const u8 sOptions);

void TryFindSaveAndLoadOrCreate() {
    save::Response response;
    bool ok = true;

    constexpr eOptions createOpts[] = { Yes, No };
    constexpr eOptions errorOpts[] = {Retry, Continue};
    constexpr eOptions eraseOpts[] = {Erase, Continue};
    constexpr eOptions confirmOpts[] = { No, Yes };
    do {
        retry:
        while (response = save::GetSaveLocation(), !response.done) {
            WaitBody();
        }

        switch (response.err) {
            case save::found:
                ok = true;  // proceed to attempt to load
                break;
            case save::missing:
                PrintContext(saveMissingText.data());
                switch (const auto choice = ChoiceWheel(createOpts, sizeof(createOpts))) {
                    case Yes:
                        // todo: try create save file
                        break;

                    case No:
                        ok = true;  // no save load

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }

                break;
            case save::no_device:
                PrintContext(saveNoDeviceText.data());
                switch (const auto choice = ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        goto retry; // re attempt

                    case Continue:
                        ok = true;  // no save load

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }

                break;
            case save::busy:
                PrintContext(saveBadErrorText.data());
                // card is reported busy, this should NOT fire under healthy circumstances
                // should show some message, that the card is busy with 'retry' or 'continue without save'
                log("card is busy, this is a serious bug that should be solved");
                switch (const auto choice = ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        goto retry; // re attempt

                    case Continue:
                        ok = true;  // no save load

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }
                break;
            case save::exists:
                PrintContext(saveBadErrorText.data());
                // should only appear on save creation, not save searching, erroneous
                // should show some message, that the card is erroring with 'retry' or 'continue without save'
                log("cannot find file because it already exists, bad error");
                switch (const auto choice = ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        goto retry; // re attempt

                    case Continue:
                        ok = true;  // no save load

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }
                break;
            case save::corrupt:
                PrintContext(saveCorruptText.data());
                // report memory card is corrupt, provide option to erase it (we need an 'are you sure')
                log("corrupt file");
                switch (const auto choice = ChoiceWheel(eraseOpts, sizeof(eraseOpts))) {
                    case Erase:
                        break;

                    case Continue:
                        ok = true;  // no save load
                        continue;

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }
                PrintContext(saveCorruptText2.data());
                switch (const auto choice = ChoiceWheel(confirmOpts, sizeof(confirmOpts))) {
                    case Yes:
                        // invoke card erase method, then retry
                        goto retry;
                        break;

                    case No:
                        ok = true;  // no save load
                        continue;

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }
                break;
            case save::full:
                PrintContext(saveBadErrorText.data());
                // should not fire on try find, if this is being seen its a big error
                // report that an unusual bug is shown, and should be reported immediately
                log("cannot find file because out of storage, bad error");
                switch (const auto choice = ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        goto retry; // re attempt

                    case Continue:
                        ok = true;  // no save load

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }
                break;
            case save::too_large:
                PrintContext(saveBadErrorText.data());
                // should not fire on load, if this is being seen its a big error
                // report that an unusual bug is shown, and should be reported immediately
                log("cannot find file because it might be too big to be loaded, bad error");
                switch (const auto choice = ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        goto retry; // re attempt

                    case Continue:
                        ok = true;  // no save load

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }
                break;
            case save::denied:
                PrintContext(saveBadErrorText.data());
                // report operation has been denied, should not occur under typical circumstances
                // report the operation is denied, and should be reported immediately
                log("cannot find file, memory card denied operation");
                switch (const auto choice = ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        goto retry; // re attempt

                    case Continue:
                        ok = true;  // no save load

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }
                break;
            case save::bad_path:
                PrintContext(saveBadErrorText.data());
                // report operation has bad path, not a user error
                // report the operation is denied, and should be reported immediately
                log("cannot find file, path is bad, bad error");
                switch (const auto choice = ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        goto retry; // re attempt

                    case Continue:
                        ok = true;  // no save load

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }
                break;
            case save::failed:
                PrintContext(saveBadErrorText.data());
                // unknown error, just let the user retry or continue without save
                log("unknown error, bad error normally");
                switch (const auto choice = ChoiceWheel(errorOpts, sizeof(errorOpts))) {
                    case Retry:
                        goto retry; // re attempt

                    case Continue:
                        ok = true;  // no save load

                    default:
                        log("unknown choice selected");
                        ok = true;  // something bad happened
                }
                break;
        }
    } while (!ok);
}

// charmapped error text message explaining status (either, success, error with choice)
void PrintContext(const u8* infoText) {
#ifdef TARGET_GC
    // draw to screen the message somewhere nice the message using rtTextBox, easy GC compute
#endif
}

eOptions ChoiceWheel(const eOptions* pOptions, const u8 sOptions) {
    // make and draw all options

    u8 opt  = 0;
    u8 port1;
    while (true) {
        // poll inputs for LR or A
        input::PollControllers(&port1, &port1); // todo:: add strobing
        if (port1 & input::LEFT) {
            if (--opt > sOptions) opt = 0;
            continue;
        }

        if (port1 & input::RIGHT) {
            if (++opt >= sOptions) opt = sOptions - 1;
            continue;
        }

        if (port1 & input::A) break;
        video::WaitForPresent();
    }

    // clear all message and options text
    // cull all Maked rtStrings
    return pOptions[opt];
}

void LoadSave() {
    save::Response response;
    while (response = save::Load(&saveData, sizeof(saveData), &response), !response.done) {
        WaitBody();
    }

    if (!ValidateSave()) {
        // print information about the save file being corrupted, handle this
    }

    return;
}

bool ValidateSave() {
    // run checks on saveData
    return true;
}

void AskCreateSave() {
    const u8 saveDevices = save::AccessibleDevices();
    // using this information, we can present what memory cards are accessible, should work on something here nicely
}