# Releasing foot for macOS

A release is a GitHub release with the notarized, universal `foot.app`
(zipped) and the Sparkle appcast, plus the Homebrew cask in
[gobijan/homebrew-tap](https://github.com/gobijan/homebrew-tap). It's
made on a Mac, with `make publish`; the signing keys stay in its
keychain.

Versions are `<upstream version>-macos.<n>`, e.g. `1.28.0-macos.1`;
the git tag is the version. `<n>` starts at 1 for every upstream
version.

## Once

1. **Developer ID:** a "Developer ID Application" certificate, with its
   private key, in the login keychain (`security find-identity -v -p
   codesigning`).
2. **Notarization:** credentials for `notarytool`, in the keychain. With
   an App Store Connect API key (Users and Access > Integrations >
   Team Keys, role Developer):

   ```sh
   xcrun notarytool store-credentials <profile> \
       --key AuthKey_<key id>.p8 --key-id <key id> --issuer <issuer id>
   ```

3. **Sparkle:** the key that updates are signed with, also in the
   keychain: `make sparkle-key`. It prints the public key; it goes in
   `macos/dist.sh` (`SPARKLE_PUBLIC_KEY`). Back the private key up, e.g.
   in a password manager:

   ```sh
   build-release/sparkle-*/bin/generate_keys --account foot-macos -x sparkle-private-key
   ```

   Without it, no update can be signed: foot.app only installs updates
   signed with the key it was released with.

## Every release

Commit and push, then:

```sh
make publish RELEASE=1.28.0-macos.1 NOTARY_PROFILE=<profile> [NOTES=notes.md]
```

Without `NOTES`, the release notes are the subjects of the commits
since the previous release. `macos/publish.sh`:

1. checks that `HEAD` is pushed, and tags it,
2. builds the universal `foot.app` with Sparkle, signs it with the
   hardened runtime, notarizes and staples it, and zips it
   (`macos/dist.sh`, also `make dist` for a build without releasing),
3. signs the zip for Sparkle, and writes the appcast,
4. pushes the tag, and creates the GitHub release with both,
5. updates the cask in gobijan/homebrew-tap (the first release creates
   that repository).

Installed copies find the update through the appcast of the latest
release
(`https://github.com/gobijan/foot-macos/releases/latest/download/appcast.xml`);
Homebrew users get it with `brew upgrade` too.
