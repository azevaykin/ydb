c-ares release procedure - how to do a release
==============================================

in the source code repo
-----------------------

- edit `RELEASE-NOTES.md` to be accurate
- edit `configure.ac`'s `CARES_VERSION_INFO`, and `CMakeLists.txt`'s
  `CARES_LIB_VERSIONINFO` set to the same value to denote the current shared
  object versioning.
- edit `include/ares_version.h` and set `ARES_VERSION_*` definitions to reflect
  the current version.
- All release tags need to be made off a release branch named `vX.Y`, where `X`
  is the Major version number, and `Y` is the minor version number. We also
  want to create an empty commit in the branch with a message, this ensures
  when we tag a release from the branch, it gets tied to the branch itself and
  not a commit which may be shared across this branch and `main`. Create the
  branch like:
```
BRANCH=1.35
git pull && \
git checkout main && \
git checkout -b v${BRANCH} main && \
git commit --allow-empty -m "Created release branch v${BRANCH}" && \
git push -u origin v${BRANCH}
```
- make sure all relevant changes are committed on the release branch
- Create a signed tag for the release using a name of `vX.Y.Z` where `X` is the
  Major version number, `Y` is the minor version number, and `Z` is the release.
  This tag needs to be created from the release branch, for example:
```
BRANCH=1.35
RELEASE=1.35.0
git checkout v${BRANCH} && \
git pull && \
git tag -s v${RELEASE} -m 'c-ares release v${RELEASE}' v${BRANCH} && \
git push origin --tags
```
- When a tag is created, it will spawn off a github action to generate a new
  draft release based on this workflow: [package.yml](https://github.com/c-ares/c-ares/blob/main/.github/workflows/package.yml).
  Wait for this workflow to complete then fetch the generated source tarball:
```
wget https://github.com/c-ares/c-ares/releases/download/v${RELEASE}/c-ares-${RELEASE}.tar.gz
```
- GPG sign the release with a detached signature. Valid signing keys are currently:
  - Daniel Stenberg <daniel@haxx.se> - 27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2
  - Brad House <brad@brad-house.com> - DA7D64E4C82C6294CB73A20E22E3D13B5411B7CA
```
gpg -ab c-ares-${RELEASE}.tar.gz
```
- Upload the generated `c-ares-${RELEASE}.tar.gz.asc` signature as a release
  asset, then unmark the release as being a draft.

in the c-ares-www repo
----------------------

- edit `index.md`, change version and date in frontmatter
- edit `changelog.md`, copy `RELEASE-NOTES.md` content
- edit `download.md`, add new version and date in frontmatter
- commit all local changes
- push the git commits

inform
------

- send an email to the c-ares mailing list. Insert the RELEASE-NOTES.md into the
  mail.
- Create an announcement in the GitHub Discussions Announcements section:
  https://github.com/c-ares/c-ares/discussions/categories/announcements

celebrate
---------

- suitable beverage intake is encouraged for the festivities
