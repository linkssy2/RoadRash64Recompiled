namespace Original {
    template<bool RGBA32 = false, bool TLUT = false>
    __forceinline void loadWord(uint8_t *TMEM, uint32_t tmemAddress, uint32_t tmemXorMask, const uint8_t *RDRAM, uint32_t textureAddress) {
        // Only sample the first two bytes in TLUT mode.
        uint32_t offsetMask;
        if constexpr (TLUT) {
            offsetMask = 0x1;
        }
        else {
            offsetMask = 0x7;
        }

        if constexpr (RGBA32) {
            // Split the lower and upper half of the word into the lower and upper half of TMEM.
            const uint32_t UpperTMEM = (RDP_TMEM_BYTES >> 1);
            TMEM[(tmemAddress + 0) ^ tmemXorMask] = RDRAM[(textureAddress + (0 & offsetMask)) ^ 3];
            TMEM[(tmemAddress + 1) ^ tmemXorMask] = RDRAM[(textureAddress + (1 & offsetMask)) ^ 3];
            TMEM[(tmemAddress + 2) ^ tmemXorMask] = RDRAM[(textureAddress + (4 & offsetMask)) ^ 3];
            TMEM[(tmemAddress + 3) ^ tmemXorMask] = RDRAM[(textureAddress + (5 & offsetMask)) ^ 3];
            TMEM[((tmemAddress + 0) ^ tmemXorMask) | UpperTMEM] = RDRAM[(textureAddress + (2 & offsetMask)) ^ 3];
            TMEM[((tmemAddress + 1) ^ tmemXorMask) | UpperTMEM] = RDRAM[(textureAddress + (3 & offsetMask)) ^ 3];
            TMEM[((tmemAddress + 2) ^ tmemXorMask) | UpperTMEM] = RDRAM[(textureAddress + (6 & offsetMask)) ^ 3];
            TMEM[((tmemAddress + 3) ^ tmemXorMask) | UpperTMEM] = RDRAM[(textureAddress + (7 & offsetMask)) ^ 3];
        }
        else {
            // Copy the entire word.
            for (uint32_t i = 0; i < 8; i++) {
                TMEM[(tmemAddress + i) ^ tmemXorMask] = RDRAM[(textureAddress + (i & offsetMask)) ^ 3];
            }
        }
    }

    template<bool RGBA32 = false, bool BLOCK = false, bool TLUT = false>
    __forceinline void loadToTMEMCommon(uint8_t *TMEM, const uint8_t *RDRAM, uint32_t textureStart, uint32_t textureStride, uint32_t tmemStart,
        uint32_t tmemStride, uint32_t wordsPerRow, uint32_t rowCount, uint32_t dxtIncrement = 0)
    {
        assert((!BLOCK || (rowCount == 1)) && "Load block must behave as if it only loads one row of data.");
        
        const uint32_t DXTSwap = 0x800;
        uint32_t textureAddress, tmemAddress, wordCount, tmemMask, tmemAdvance;
        if constexpr (RGBA32) {
            tmemMask = RDP_TMEM_MASK16;
            tmemAdvance = 0x4;
        }
        else {
            tmemMask = RDP_TMEM_MASK8;
            tmemAdvance = 0x8;
        }

        uint32_t textureAdvance;
        if constexpr (TLUT) {
            textureAdvance = 0x2;
        }
        else {
            textureAdvance = 0x8;
        }

        uint32_t tmemXorMask = 0x0;
        uint32_t dxtCounter = 0x0;
        auto loadWordStep = [&]() {
            if constexpr (BLOCK) {
                dxtCounter += dxtIncrement;
                while (dxtCounter >= DXTSwap) {
                    tmemAddress = (tmemAddress + tmemStride) & tmemMask;
                    dxtCounter -= DXTSwap;
                    tmemXorMask ^= 0x4;
                }
            }

            textureAddress += textureAdvance;
            tmemAddress = (tmemAddress + tmemAdvance) & tmemMask;
        };
        
        uint32_t textureAddressRow = textureStart;
        uint32_t tmemAddressRow = tmemStart & tmemMask;
        auto loadRowStep = [&]() {
            tmemAddressRow = (tmemAddressRow + tmemStride) & tmemMask;
            textureAddressRow += textureStride;
            tmemXorMask ^= 0x4;
        };

        // As an optimization for large texture loads that end up wrapping around in TMEM, we skip rows that have no effect in the final result.
        if ((tmemStride > 0) && ((wordsPerRow * tmemAdvance) <= tmemStride)) {
            int32_t rowsToSkip = rowCount - (tmemMask + tmemStride) / tmemStride;
            if (rowsToSkip > 0) {
                tmemAddressRow += (tmemAddressRow + tmemStride * rowsToSkip) & tmemMask;
                textureAddressRow += textureStride * rowsToSkip;
                tmemXorMask = (rowsToSkip & 0x1) << 2;
                rowCount -= rowsToSkip;
            }
        }

        while (rowCount > 0) {
            textureAddress = textureAddressRow;
            tmemAddress = tmemAddressRow;
            wordCount = wordsPerRow;
            while (wordCount > 0) {
                loadWord<RGBA32, TLUT>(TMEM, tmemAddress, tmemXorMask, RDRAM, textureAddress);
                loadWordStep();
                wordCount--;
            }

            loadRowStep();
            rowCount--;
        }
    }


}
