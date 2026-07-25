#pragma once
#include <cstddef>

// ---------------------------------------------------------------------------
// Template for webhook_config.h, which report.cpp actually includes.
// webhook_config.h itself is gitignored -- never commit it.
//
// Holds the vfxd-sins-report-relay Cloudflare Worker's URL, NOT a raw
// Discord webhook -- the relay holds the real Discord webhook server-side
// (as a Worker secret) and this addon never sees it. See that repo's
// README for deploying the relay itself.
//
// Don't hand-edit the byte array below -- regenerate it instead, from the
// repo root:
//
//     python3 tools/generate_webhook_config.py "https://<your-worker>.<your-subdomain>.workers.dev" > src/webhook_config.h
//
// That produces a real src/webhook_config.h next to this file (gitignored,
// untracked, stays only on your machine).
//
// What this obfuscation is and isn't:
// The URL below is XOR-obfuscated with a fixed key (see kWebhookXorKey in
// report.cpp), NOT encrypted -- its only job is to keep the URL from
// showing up as one plain readable string in a `strings`/hex-editor pass
// over the built DLL. It does nothing against a debugger, a WinHTTP-call
// hook, or a network-level proxy watching the actual (TLS-protected) POST
// go out -- DecodeWebhookUrl() has to reconstruct the real URL in memory
// right before the request either way. Unlike a raw Discord webhook, this
// URL isn't a credential -- the relay is rate-limited and does nothing but
// validate+forward -- so the obfuscation here is about not handing the
// endpoint to casual scraping, not about protecting a secret.
//
// The array below decodes to the placeholder URL
// "https://discord.com/api/webhooks/REPLACE_WITH_YOUR_WEBHOOK_ID/REPLACE_WITH_YOUR_WEBHOOK_TOKEN"
// -- harmless to leave committed as-is; it isn't a real webhook or a real
// relay URL.
// ---------------------------------------------------------------------------
inline constexpr unsigned char kWebhookUrlXor[] = {
    0x32, 0x48, 0xe5, 0x0e, 0x5e, 0xf2, 0x3e, 0x75, 0x58, 0xf8, 0x0d, 0x4e,
    0xa7, 0x63, 0x3e, 0x12, 0xf2, 0x11, 0x40, 0xe7, 0x70, 0x2a, 0x55, 0xbe,
    0x09, 0x48, 0xaa, 0x79, 0x35, 0x53, 0xfa, 0x0d, 0x02, 0x9a, 0x54, 0x0a,
    0x70, 0xd0, 0x3d, 0x68, 0x97, 0x46, 0x13, 0x68, 0xd9, 0x21, 0x74, 0x87,
    0x44, 0x08, 0x63, 0xc6, 0x3b, 0x6f, 0x80, 0x5e, 0x15, 0x77, 0xce, 0x37,
    0x69, 0xe7, 0x43, 0x1f, 0x6c, 0xdd, 0x3f, 0x6e, 0x8d, 0x4e, 0x0d, 0x75,
    0xc5, 0x36, 0x72, 0x91, 0x5e, 0x0f, 0x6e, 0xce, 0x29, 0x68, 0x8a, 0x59,
    0x15, 0x73, 0xda, 0x21, 0x79, 0x87, 0x5a, 0x1f, 0x72,
};
inline constexpr size_t kWebhookUrlXorLen = 93;
