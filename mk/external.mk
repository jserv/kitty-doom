# External file download rules

# Detect available download tool (using command -v for better POSIX compliance)
ifeq ($(shell command -v wget >/dev/null 2>&1; echo $$?),0)
    DOWNLOAD_CMD = wget -q -O
else ifeq ($(shell command -v curl >/dev/null 2>&1; echo $$?),0)
    DOWNLOAD_CMD = curl -fsSL --max-time 30 -o
else
    $(error Neither wget nor curl found. Please install one of them.)
endif

# Detect available checksum tool
ifeq ($(shell command -v shasum >/dev/null 2>&1; echo $$?),0)
    SHA256_CMD = shasum -a 256 -c -
else ifeq ($(shell command -v sha256sum >/dev/null 2>&1; echo $$?),0)
    SHA256_CMD = sha256sum -c -
else
    $(warning No shasum or sha256sum found; skipping integrity check)
    SHA256_CMD =
endif


# DOOM1.WAD (shareware version)
# Note: PureDOOM expects lowercase "doom1.wad" on case-sensitive filesystems (Linux).
# The Makefile will create a symlink automatically via the check-wad-symlink target.
DOOM1_ZIP_URL = https://archive.org/download/doom-wads/Doom%20%28v1.9%29.zip
DOOM1_ZIP = doom1.zip
DOOM1_WAD = DOOM1.WAD
DOOM1_WAD_SHA256 = ff2c301b8719465a6e386a512bfa319931b7f64ea517d337c5a47afe03951902

$(DOOM1_WAD):
	$(VECHO) "  GET\t$(DOOM1_ZIP)\n"
	$(Q)$(DOWNLOAD_CMD) $(DOOM1_ZIP) $(DOOM1_ZIP_URL)
	$(VECHO) "  UNZIP\t$@\n"
	$(Q)unzip -q -o -j $(DOOM1_ZIP) "DOOM.WAD" -d .
	$(Q)mv DOOM.WAD $@

ifdef SHA256_CMD
	$(VECHO) "  CHK\t$@ (SHA256)\n"
	$(Q)echo "$(DOOM1_WAD_SHA256)  $@" | $(SHA256_CMD) >/dev/null || \
		{ echo "Error: Checksum mismatch for $@"; rm -f "$@"; false; }
endif
	$(VECHO) "  RM\t$(DOOM1_ZIP)\n"
	$(Q)rm -f $(DOOM1_ZIP)


# PureDOOM.h download rule
PUREDOOM_URL = https://raw.githubusercontent.com/Daivuk/PureDOOM/master/PureDOOM.h
PUREDOOM_HEADER = src/PureDOOM.h

$(PUREDOOM_HEADER):
	$(VECHO) "  GET\t$@\n"
	$(Q)$(DOWNLOAD_CMD) $@ $(PUREDOOM_URL)

# miniaudio.h download rule (single-header audio library)
MINIAUDIO_URL = https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h
MINIAUDIO_HEADER = src/miniaudio.h

$(MINIAUDIO_HEADER):
	$(VECHO) "  GET\t$@\n"
	$(Q)$(DOWNLOAD_CMD) $@ $(MINIAUDIO_URL)

# Clean external files
.PHONY: clean-external
clean-external:
	$(VECHO) "  CLEAN\t\texternal files\n"
	$(Q)rm -f $(DOOM1_WAD) doom1.wad $(DOOM1_ZIP) $(PUREDOOM_HEADER) $(MINIAUDIO_HEADER)
