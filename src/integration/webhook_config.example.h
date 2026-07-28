//################################################################################
// webhook_config.example.h
//--------------------------------------------------------------------------------
// kWebhookUrlXor      XOR-obfuscated relay URL bytes (placeholder as committed)
// kWebhookUrlXorLen   byte length of kWebhookUrlXor
//--------------------------------------------------------------------------------
// Template for webhook_config.h, which report.cpp actually includes.
// webhook_config.h itself is gitignored -- never commit it.
//
// Don't hand-edit the byte array below -- regenerate it instead, from the
// repo root:
//
//     python3 tools/generate_webhook_config.py "https://<your-worker>.<your-subdomain>.workers.dev" > src/integration/webhook_config.h
//
// That produces a real src/integration/webhook_config.h next to this file
// (gitignored, untracked, stays only on your machine).
//
// What this obfuscation is and isn't: the URL below is XOR-obfuscated with
// a fixed key (see kWebhookXorKey in webhook_report.cpp), NOT encrypted --
// its only job is to keep the URL from showing up as one plain readable
// string in a `strings`/hex-editor pass over the built DLL. It does
// nothing against a debugger, a WinHTTP-call hook, or a network-level
// proxy watching the actual (TLS-protected) POST go out --
// DecodeWebhookUrl() has to reconstruct the real URL in memory right
// before the request either way. Unlike a raw Discord webhook, this URL
// isn't a credential -- the relay is rate-limited and does nothing but
// validate+forward -- so the obfuscation here is about not handing the
// endpoint to casual scraping, not about protecting a secret.
//
// The array below decodes to the placeholder URL
// "https://example-worker.example.workers.dev" -- harmless to leave committed
// as-is; it isn't a real relay URL.
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