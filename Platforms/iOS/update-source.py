#!/usr/bin/env python3
"""Point one app's entry in a SideStore/AltStore source at a freshly published IPA.

Runs on the machine hosting the source, invoked by build-and-deploy.sh.

It exists because the app entry has to be found by bundle identifier. The
publish step used to be three `sed -E` substitutions over the whole file -
`"version"`, `"versionDate"`, `"size"` - which is exact enough while a source
lists exactly one app and silently wrong the moment it lists two: publishing
the editor would rewrite whichever entry came first, i.e. the game's. There is
no way to make a line-oriented substitution mean "the object with this
bundleIdentifier", so this parses the JSON instead.

Refusing is deliberate when no entry matches. The alternative - appending a new
app object - would be guessing at fields this cannot know (name, subtitle,
developer, icon and screenshot URLs, the download URL's public hostname), and a
half-populated entry in a live source is worse than a clear failure.
"""

import json
import sys


def main():
    if len(sys.argv) != 6:
        sys.exit(f"usage: {sys.argv[0]} SOURCE_PATH BUNDLE_ID VERSION DATE SIZE")

    path, bundle_id, version, version_date, size = sys.argv[1:6]
    size = int(size)

    with open(path) as handle:
        source = json.load(handle)

    apps = source.get("apps")

    if not isinstance(apps, list):
        sys.exit(f"{path} has no 'apps' array")

    app = next((a for a in apps if a.get("bundleIdentifier") == bundle_id), None)

    if app is None:
        listed = ", ".join(a.get("bundleIdentifier", "?") for a in apps) or "none"
        sys.exit(
            f"{path} has no app with bundleIdentifier {bundle_id} (it lists: {listed}).\n"
            f"Add an entry for it before publishing; this will not invent one."
        )

    # Both source schemas, because which one a client reads depends on its
    # version: the flat fields are the legacy AltStore layout, `versions[0]` is
    # the current one. Whichever this file actually uses gets updated and the
    # other is left alone rather than created.
    if "version" in app:
        app["version"] = version
    if "versionDate" in app:
        app["versionDate"] = version_date
    if "size" in app:
        app["size"] = size

    versions = app.get("versions")

    if isinstance(versions, list) and versions:
        latest = versions[0]
        latest["version"] = version
        latest["date"] = version_date
        latest["size"] = size

    with open(path, "w") as handle:
        json.dump(source, handle, indent=2)
        handle.write("\n")

    print(f"{bundle_id}: version {version}, {size} bytes")


if __name__ == "__main__":
    main()
