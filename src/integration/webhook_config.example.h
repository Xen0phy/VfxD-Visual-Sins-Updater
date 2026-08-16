//################################################################################
// webhook_config.example.h
//--------------------------------------------------------------------------------
// kWebhookUrlXor      XOR-obfuscated relay URL bytes (placeholder as committed)
// kWebhookUrlXorLen   byte length of kWebhookUrlXor
//--------------------------------------------------------------------------------
// Template for webhook_config.h, which report.cpp actually includes.
// webhook_config.h itself is gitignored -- never commit it.
//
// Don't hand-edit the byte array below -- regenerate it instead, from the repo
// root:
//
//     python3 tools/generate_webhook_config.py "https://<your-worker>.<your-subdomain>.workers.dev" > src/integration/webhook_config.h
//
// XOR-obfuscated with a fixed key (see kWebhookXorKey in webhook_report.cpp),
// NOT encrypted -- keeps the URL out of a strings/hex-editor pass over the DLL
// only, not from a debugger, hook, or network proxy (DecodeWebhookUrl()
// reconstructs it in memory before every request regardless). Not a credential
// -- the relay is rate-limited, validate+forward only -- so this guards
// against casual scraping, not a secret.
//
// Decodes to the placeholder "https://example-worker.example.workers.dev",
// harmless to leave committed as-is.
//--------------------------------------------------------------------------------

#pragma once

#include <cstddef>

inline constexpr unsigned char kWebhookUrlXor[] = {
    0x32, 0x48, 0xe5, 0x0e, 0x5e, 0xf2, 0x3e, 0x75, 0x59, 0xe9, 0x1f, 0x40,
    0xb8, 0x7d, 0x3f, 0x11, 0xe6, 0x11, 0x5f, 0xa3, 0x74, 0x28, 0x12, 0xf4,
    0x06, 0x4c, 0xa5, 0x61, 0x36, 0x59, 0xbf, 0x09, 0x42, 0xba, 0x7a, 0x3f,
    0x4e, 0xe2, 0x50, 0x49, 0xad, 0x67,
};
inline constexpr size_t kWebhookUrlXorLen = 42;