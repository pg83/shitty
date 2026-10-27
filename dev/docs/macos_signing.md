# Signing and notarizing the macOS release

`.github/workflows/release.yml`'s `sign-notarize-darwin` job code-signs
`Shitty.app`/`Pretty.app` with a Developer ID Application certificate,
submits them to Apple for notarization, and staples the resulting ticket to
each bundle. Without that, a downloader who got the release via a browser
(which sets the quarantine flag) sees Gatekeeper's "cannot be opened because
the developer cannot be verified" dialog on first launch.

The certificate and API key used here are **dedicated to this project** —
not reused from anything else. That matters for revocation: a Developer ID
Application certificate is not scoped to a single app, so whoever holds it
can sign anything under that identity, and revoking it (because a secret
leaked, or for any other reason) invalidates every binary ever signed with
it. Keeping this repo's signing identity separate means an incident here
stays contained to this repo's own releases.

## What this repo builds

`st`/`pt` are built directly by `./build` (see `dev/build_brew_macos.sh`),
with no Electron or other app-packager involved. `dev/package_macos_app.sh`
wraps the result in a minimal `.app` bundle (`Info.plist`, an `.icns`
generated from the same SVG the Linux `.desktop` entry uses). The release
workflow then calls `codesign` and `xcrun notarytool`/`stapler` directly.

Two release artifact shapes exist side by side. They are signed separately:
a bundle signature binds its executable to `Contents/Info.plist`, so copying
that executable out of the bundle would leave an invalid standalone
signature that macOS refuses to execute.

- **`Shitty.app`/`Pretty.app`** (`Shitty-darwin-arm64.tar.gz` /
  `Pretty-darwin-arm64.tar.gz` on the release) — signed, notarized, and
  **stapled**. Gatekeeper accepts these offline.
- **loose `st`/`pt`** (`st-darwin-arm64.tar.gz` / `pt-darwin-arm64.tar.gz`)
  — feeds the Homebrew tap (`pg83/homebrew-tap`, a separate repository this
  workflow cannot update in lockstep), unchanged in shape from every prior
  release. `sign-notarize-darwin` signs these directly, with no dependency
  on a bundle `Info.plist`, verifies each signature, and executes both
  binaries after signing. They go into the same notarization submission as
  the apps. A loose Mach-O executable cannot be **stapled** — `stapler` only
  takes a `.app`/`.pkg`/`.dmg` — so Gatekeeper checks these online against
  Apple's notary service by the binary's own code signature on first run
  instead. That needs network access once; everything after is cached.

## One-time setup

1. **A Developer ID Application certificate**, from an enrolled Apple
   Developer Program membership. Xcode → Settings → Accounts → your team →
   Manage Certificates → **+** → Developer ID Application (or the portal:
   Certificates, Identifiers & Profiles → Certificates → **+**, from a CSR
   made in Keychain Access → Certificate Assistant → Request a Certificate).
   No provisioning profile is needed — Developer ID apps aren't sandboxed or
   profile-scoped.

   **This one step cannot be delegated or automated.** Apple restricts
   creating a Developer ID Application (or Installer) certificate to the
   Apple Developer Program membership's **Account Holder** specifically —
   confirmed here by both a direct App Store Connect API call (`POST
   /v1/certificates` with a non-Account-Holder API key: `403
   FORBIDDEN_ERROR — "This operation can only be performed by the Account
   Holder"`) and by fastlane `match`/`cert`, which hit the same restriction
   through the same endpoint. No API key role, and no automation around one,
   changes this — it has to be the Account Holder, once, through Xcode or
   the web portal.
2. **An App Store Connect API key** scoped to the minimal role notarization
   needs (Developer), made at App Store Connect → Users and Access →
   Integrations → **+**. Apple lets you download the `.p8` exactly once.

## Export and set the secrets

`dev/set_macos_signing_secrets.sh` reads this repo's local SOPS store
(below) and pushes it to the target repository's secrets — no plaintext
file ever named on the command line. It needs the ability to decrypt
`.keys/secrets.enc.yaml` (the age private key that made it) and Admin
access to the target repository, same as `gh secret set` itself:

```sh
dev/set_macos_signing_secrets.sh --repo OWNER/NAME
```

It always sets the certificate pair (the store always carries
`devid_p12_password`) and additionally sets the ASC pair when the store
also carries `asc_key_id`/`asc_issuer_id`/`asc_key_p8`.

On a machine that has the files but not this repo's SOPS store — e.g.
pg83's own machine, which cannot decrypt a store encrypted to someone
else's age key — set them by hand instead, one secret at a time:

```sh
gh secret set APPLE_DEVELOPER_ID_CERTIFICATE_BASE64 < <(base64 -i developer-id.p12)
gh secret set APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD --body "<the .p12 export password>"

gh secret set APP_STORE_CONNECT_API_KEY_BASE64 < <(base64 -i AuthKey_XXXXXXXXXX.p8)
gh secret set APP_STORE_CONNECT_API_KEY_ID --body "<the key ID>"
gh secret set APP_STORE_CONNECT_API_ISSUER_ID --body "<the issuer ID>"
```

| Secret | What it is |
| --- | --- |
| `APPLE_DEVELOPER_ID_CERTIFICATE_BASE64` | base64 of the Developer ID Application `.p12` |
| `APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD` | that `.p12`'s export password |
| `APP_STORE_CONNECT_API_KEY_BASE64` | base64 of the App Store Connect API `.p8` |
| `APP_STORE_CONNECT_API_KEY_ID` | that key's ID |
| `APP_STORE_CONNECT_API_ISSUER_ID` | the issuer ID |

No `APPLE_TEAM_ID` secret is needed — plain `codesign`/`notarytool` derive
the team from the certificate and API key themselves.

## Local secret store

The `.p12` and its export password also live in a local, SOPS-encrypted
store at `.keys/`, so they survive independent of any one machine's GitHub
CLI session or Keychain. `.sops.yaml` at the repo root names the recipient
(an age key) and is safe to commit — it holds no secret, only the public
recipient config. `.keys/` itself is excluded twice over: this repo's
whitelist `.gitignore` (its root `/*` rule leaves out anything not
explicitly un-ignored) and, as a second local-only safety net,
`.git/info/exclude`.

```sh
export SOPS_AGE_KEY_FILE=~/.config/sops/age/keys.txt
sops --config /dev/null -d .keys/secrets.enc.yaml   # read the password/metadata
```

`.keys/shitty-devid.p12` sits beside it as a plain (not SOPS-encrypted)
file, protected by the same gitignore layering — only its export password,
in `secrets.enc.yaml`, is actually encrypted at rest.

## Degrading gracefully when secrets are absent

Every step in `sign-notarize-darwin` checks for its secrets with
`if [ -z … ]`, not `set -e` alone, and warns-and-returns rather than fails
when they are missing — a fork gets none of this repo's secrets, and a
secret can be rotated away. That leaves the bundle with whatever signature
the linker already applied (an ad-hoc one on Apple Silicon, which is
required just to execute, not a Developer ID one) — the same thing this
repo shipped before this pipeline existed, not a failed release.

## Local reproduction

```sh
dev/package_macos_app.sh Shitty com.pg83.shitty st bin/st/shitty.svg .build-darwin/st .build-darwin/Shitty.app
codesign --force --options runtime --timestamp --sign "Developer ID Application: …" .build-darwin/Shitty.app
codesign --force --options runtime --timestamp --sign "Developer ID Application: …" .build-darwin/st
codesign --verify --deep --strict .build-darwin/Shitty.app
codesign --verify --strict .build-darwin/st
.build-darwin/st -version
mkdir -p /tmp/shitty-submit
cp -R .build-darwin/Shitty.app /tmp/shitty-submit/
cp .build-darwin/st /tmp/shitty-submit/
ditto -c -k --keepParent /tmp/shitty-submit /tmp/Shitty.zip
xcrun notarytool submit /tmp/Shitty.zip --key-id … --issuer … --key … --wait
xcrun stapler staple .build-darwin/Shitty.app
spctl -a -vv -t execute .build-darwin/Shitty.app   # expect: accepted, source=Notarized Developer ID
```
