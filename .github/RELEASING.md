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

   The first `make publish` asks for `<profile>`, and remembers it.

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

```sh
make publish
```

It asks for everything, and publishes nothing before the last question.
`macos/publish.sh`:

1. checks what a release needs: everything committed and pushed (it
   offers to push), CI passed on this commit (it offers to wait for
   it), the Developer ID certificate, the notarization profile (asked
   for the first time, then remembered in the clone's git config,
   `foot.notaryprofile`) and Sparkle's key,
2. suggests the version: the next `-macos.<n>` of the upstream version
   in `meson.build`,
3. shows the release notes, the subjects of the commits since the
   previous release, and offers to edit them (`$EDITOR`); write them
   for the users, Sparkle's update window shows them,
4. asks whether to publish, then tags `HEAD`,
5. builds the universal `foot.app` with Sparkle, signs it with the
   hardened runtime, notarizes and staples it, and zips it
   (`macos/dist.sh`, also `make dist` for a build without releasing),
6. signs the zip for Sparkle, and writes the appcast,
7. pushes the tag, and creates the GitHub release with both,
8. updates the cask in gobijan/homebrew-tap (the first release creates
   that repository).

If it fails, fix the cause and run `make publish` again: the tag is
only pushed once `foot.app` is built and notarized, and the edited
release notes are kept (`build-release/release-notes-<version>.md`).

Installed copies find the update through the appcast of the latest
release
(`https://github.com/gobijan/foot-macos/releases/latest/download/appcast.xml`);
Homebrew users get it with `brew upgrade` too.
