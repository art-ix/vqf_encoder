#include "stdafx.h"

DECLARE_COMPONENT_VERSION(
    "TwinVQ encoder",
    "1.0",
    "Independent TwinVQ/VQF encoder for foobar2000.\n"
    "Companion to foo_input_vqf (https://github.com/art-ix/vqf_decoder).\n"
    "Copyright (c) 2026 vqf_encoder authors. MIT License.\n"
    "Co-authored by Grok (xAI).\n"
    "CLI: vqf_encode.exe  —  Converter: encoder vqf_encode.exe, extension vqf, parameters -b 96 %s %d\n"
    "Known NTT TwinVQ patent families expired ~2015-2020; this is not legal advice.");

VALIDATE_COMPONENT_FILENAME("foo_enc_vqf.dll");
