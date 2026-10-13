class ShellVersion {
  constructor(release = null, version = null) {
    if (release) {
      this.release = release;
      this.version = release.tag_name;
      this.versionName = release.name;
      this.versionTag = release.tag_tag;
    } else if (version) {
      this.release = null;
      this.version = version;
      this.versionName = null;
      this.versionTag = null;
    } else {
      throw new Error('Either release or version must be provided');
    }

    this.versionParts = this.parseVersion(this.version);
    this.versionMajor = this.versionParts ? this.versionParts[0] : null;
    this.versionMinor = this.versionParts ? this.versionParts[1] : null;
    this.versionPatch = this.versionParts ? this.versionParts[2] : null;
    this.versionIncremental = this.versionParts ? this.versionParts[3] : null;
  }

  parseVersion(version) {
    if (!version) {
      return null;
    }

    let v = version;

    if (v.indexOf("v") === 0) {
      v = v.substring(1);
    }

    const [mainVer, incrementalVer] = v.split('-');
    const versionParts = mainVer.split('.').map(i => parseInt(i, 10));

    if (incrementalVer) {
      const [prefix, verStr] = incrementalVer.split('.');
      let incremental = parseInt(verStr, 10);

      if (prefix === 'beta') {
        // we couldn't have 2^16 alpha versions?
        incremental <<= 16;
      }

      versionParts.push(incremental);
    }

    return versionParts;
  }

  // otherVersion is a ShellVersion or a version string such as "v1.2.0-beta.1"
  isGreater(otherVersion, checkIncremental) {
    const otherVersionParts = otherVersion instanceof ShellVersion
      ? otherVersion.versionParts
      : this.parseVersion(otherVersion);

    if (!this.versionParts || !otherVersionParts) {
      return false;
    }

    for (let i = 0; i < Math.min(checkIncremental && 4 || 3, this.versionParts.length, otherVersionParts.length); i++) {
      if (this.versionParts[i] > otherVersionParts[i]) {
        return true;
      } else if (this.versionParts[i] < otherVersionParts[i]) {
        return false;
      }
    }

    return false;
  }
}

export default ShellVersion;
