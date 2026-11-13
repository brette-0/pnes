Introduction
============

Dependencies
------------

``platform-nes`` requires a few libraries, some of which are fetched at compile time for static compilation.

1. ``intsh`` - integer type shorthands under the ``br0`` namespace for easy concise type specification and usage.
2. ``tuple`` - a lightweight clone of ``std::tuple`` under ``br0::tuple`` to gain compile time advantages for ``llvm-mos-sdk``.
3. ``SDL3`` - used for WASM, Linux, Windows and MacOS builds handling input, audio and video.
4. ``devkitpro`` - used for nintendo ARM/PPC targets
5. ``libusb1.0`` - used for USB connectivity on linux


Compiling
---------

**1. Compiling For NES**

Ensure that you have the ``llvm-mos-sdk`` installed and are using clang with c++23 and the compilation variable is set in
your ``local.cmake`` file. The rest of the dependencies should be included via the ``CMakeLists``.

**2. Compiling for Windows/Mac/Linux**

If you are compiling for Linux, ``SDL3`` is linked against dynamically, Otherwise ``SDL3`` is linked against statically.
``libusb1.0`` is also linked against dynamically as its expected to be on your system.

**3. Compiling for Web Assembly**

I use docker and CI for this, you can copy the CI if you want but of course you'll need to set up your host and I won't
give you the cloudflare token for the official demo.

**4. Compiling for OGC/Wii/3ds/WiiU/Switch**

You need to set up ``devkitpro`` on your machine or in your CI. ``local.cmake`` does not ask for the paths here as
compiler variables and the compilation workflow will expect the correct variables to exist OS-level. On some IDEs this
isn't trivial but you can likely find a way.

Audio
----------
Every song that *should* exist in the NES target should be symmetrical for a WAV file for SDL3 targets. This can be
decalred with:

.. code-block:: c

        // SDL3

        TRACKS(
            {.fp ="tracks/pc_audio.wav", .loop_start = 0}
        );

.. code-block:: c

        // NES

        extern const uint8_t _music_data_mega_man_2[];

        TRACKS(_music_data_mega_man_2);


Character ROM
-------------

``platform-nes`` promotes symbolic referencable assets. The CHR ROM layout is declared up front in a
``config.toml`` and packed by ``blobman`` (``tools/blobman``) into one padded blob, beside which it emits a
header of symbolic tile ids:

.. code-block:: toml

    [manifest]
    size            = 0x4000    # 16KiB CHR

    [normalSprites]
    LMA             = 0x0000    # where it is packed in the blob
    size            = 0x0800
    includes        = [
        "./assets/sprites/enemies/mushlet/mushlet_standing.chr",
        "./assets/sprites/wand.chr"
    ]

.. code-block:: cp

    namespace blob {
        namespace normalSprites {
            // tile indices into character rom; see ppu::ChrWindow for
            // turning one into the index the ppu reads
            constexpr unsigned short mushlet_standing = 0x0000;
            constexpr unsigned short wand             = 0x0018;
            constexpr unsigned long  __lma            = 0x0000; // byte offset, used for BankOf()
            constexpr unsigned long  __size           = 0x0800; // bytes the entry occupies
        }
    }

Those indices are ordinary ``constexpr`` constants, so they work in compile time tables. They are indices into
CHARACTER ROM, not into a pattern table: where the PPU sees a tile is not a property of how it was packed, so a
``ppu::ChrWindow`` -- the set's Character ROM origin, and the tile index it is banked in at -- is what translates
one into the index the hardware reads.

.. code-block:: cp

    constexpr ppu::ChrWindow sprites{blob::normalSprites::__lma / 16, 0x00};

    const u8 tile = sprites + blob::normalSprites::wand;

Alongside them each entry carries ``__lma``, where it was packed in the CHR image, which is what a mapper banks
it in by:

.. code-block:: cp

    mmc3::SwitchCHRBank<mmc3::BankOf<blob::ui::__lma>()>(mmc3::chr3Control);

``BankOf`` turns the load address into a bank number, and both are compile time constants, so the call folds
down to the mapper's own register writes with literal operands. Which register a set goes to is the game's own
knowledge: under ``mmc3::chrMode::fineFirst`` R2-R5 are the 1 KiB windows at PPU ``$0000``/``$0400``/``$0800``/
``$0C00`` and R0/R1 the 2 KiB ones at ``$1000``/``$1800``. Passing the bank as a template argument, as above, is
what lets ``SwitchCHRBank`` check it -- R0/R1 round an odd 1 KiB bank down, so a set bound for one of them has
to be packed 2 KiB-aligned.

Getting the blob into the ROM is one translation unit that ``#embed``\ s it into the CHR section and states its
size -- see ``demo/src/graphics/chr_rom.cpp``.

Boilerplate
-----------

To create your program, you must use the ``RESET`` and ``NMI`` macros like so:

.. code-block:: c

    RESET {
        // main code here
    }


    NMI {
        // post render code here
    }

If you wish to create a trampoline (to have multiple interrupt flavour handlers):

.. code-block:: c

    interrupt nmi_handler() {
        // my interrupt code
    }

    NAKED_NMI {
        JUMP(nmi_handler) // or other function pointer, to create indirect jump
    }

.. warning::
    You absolutely must use the ``NAKED_NMI`` for this approach, as otherwise you **will** experience a stack overflow.
    This is because the ``NMI`` macro will generate llvm-mos interrupt boilerplate that the inline assembly macro
    ``JUMP`` will make the exit of the boiler plate (which handles stack) unreachable.

.. warning::
    As of writing this, llvm-mos has a 'c-stack' for both standard codeflow and interrupts. Entering an interrupt can
    be quite costly and will use memory in a fixed region and may write into reserved memory if an overlap is present.
    This can be mitigated by moving the ``c_writable`` segment elsewhere in the linker script.

Once you have this up and running, ensure you can build for both PC and NES and refer to the docs for other areas.
I hope you enjoy using ``platform-nes``.

Program ROM Bankswitching (VRC1)
--------------------------------


Character ROM Bankswitching (VRC1)
----------------------------------

.. _docker: https://www.docker.com
.. _famistudio: https://github.com/BleuBleu/FamiStudio