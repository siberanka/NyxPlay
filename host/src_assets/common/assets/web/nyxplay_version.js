/**
 * Releases of the NyxPlay monorepo. Host builds are attached to tags named
 * `vYYYY.MM.DD.BUILD` (all components) or `host-vYYYY.MM.DD.BUILD`.
 * GitHub is queried first; the GitLab mirror is only used when GitHub fails or has no usable release.
 */
export const RELEASES_API = "https://api.github.com/repos/siberanka/NyxPlay/releases?per_page=20";
export const RELEASES_API_FALLBACK = "https://gitlab.com/api/v4/projects/siberanka%2FNyxPlay/releases?per_page=20";
export const RELEASES_PAGE = "https://github.com/siberanka/NyxPlay/releases";

const TRUSTED_HOSTS = ["github.com", "gitlab.com"];
const MAX_BODY_LENGTH = 20000;
const FETCH_TIMEOUT_MS = 10000;

/**
 * Return `url` when it points into the project's own repository on GitHub or GitLab over HTTPS,
 * otherwise the fixed releases page.
 */
export function safeReleaseUrl(url) {
  try {
    if (typeof url !== "string") {
      return RELEASES_PAGE;
    }
    const parsed = new URL(url);
    if (parsed.protocol !== "https:" || !TRUSTED_HOSTS.includes(parsed.hostname) || parsed.port !== ""
      || parsed.username !== "" || parsed.password !== "") {
      return RELEASES_PAGE;
    }
    if (parsed.pathname !== "/siberanka/NyxPlay" && !parsed.pathname.startsWith("/siberanka/NyxPlay/")) {
      return RELEASES_PAGE;
    }
    return parsed.href;
  } catch (e) {
    return RELEASES_PAGE;
  }
}

function asText(value, maxLength) {
  return typeof value === "string" ? value.slice(0, maxLength) : "";
}

/**
 * Normalise a GitHub (or GitLab mirror) release object to the fields used by the UI:
 * tag_name, name, body, html_url, prerelease and draft. The link is restricted to the project's repository.
 */
export function normalizeRelease(release) {
  if (!release || typeof release !== "object" || typeof release.tag_name !== "string" || !release.tag_name) {
    return null;
  }
  const tag = release.tag_name.slice(0, 100);
  return {
    tag_name: tag,
    name: asText(release.name, 200) || tag,
    body: asText(release.body ?? release.description, MAX_BODY_LENGTH),
    html_url: safeReleaseUrl(release.html_url ?? (release._links && release._links.self)),
    prerelease: Boolean(release.prerelease ?? release.upcoming_release) || /-(alpha|beta|rc)/i.test(tag),
    draft: Boolean(release.draft),
  };
}

async function fetchJson(url, fetchImpl) {
  const controller = typeof AbortController === "function" ? new AbortController() : null;
  const timer = controller ? setTimeout(() => controller.abort(), FETCH_TIMEOUT_MS) : null;
  try {
    const response = await fetchImpl(url, {
      headers: { Accept: "application/json" },
      referrerPolicy: "no-referrer",
      signal: controller ? controller.signal : undefined,
    });
    if (!response || !response.ok) {
      return null;
    }
    return await response.json();
  } catch (e) {
    return null;
  } finally {
    if (timer) {
      clearTimeout(timer);
    }
  }
}

/**
 * Pick the newest release, by version, from a list of normalised releases.
 */
function newest(releases) {
  let best = null;
  for (const release of releases) {
    const candidate = new NyxPlayVersion(release, null);
    if (!candidate.versionParts) {
      continue;
    }
    if (!best || candidate.isGreater(best)) {
      best = candidate;
    }
  }
  return best;
}

/**
 * Query the releases and return `{ stable, preRelease }` (NyxPlayVersion objects or null).
 * Returns null when no source could be reached, so that callers can stay silent instead of
 * claiming that the installed version is current.
 */
export async function fetchLatestVersions(fetchImpl = (typeof fetch === "function" ? fetch : null)) {
  if (!fetchImpl) {
    return null;
  }
  let reachable = false;
  for (const url of [RELEASES_API, RELEASES_API_FALLBACK]) {
    const body = await fetchJson(url, fetchImpl);
    if (!Array.isArray(body)) {
      continue;
    }
    reachable = true;
    const releases = body.slice(0, 50).map(normalizeRelease).filter(r => NyxPlayVersion.isHostRelease(r) && !r.draft);
    const stable = newest(releases.filter(r => !r.prerelease));
    const preRelease = newest(releases.filter(r => r.prerelease));
    if (stable || preRelease) {
      return { stable, preRelease };
    }
  }
  return reachable ? { stable: null, preRelease: null } : null;
}

class NyxPlayVersion {
  static isHostRelease(release) {
    if (!release || typeof release.tag_name !== "string") {
      return false;
    }
    return /^(host-)?v?\d{4}\.\d{1,2}\.\d{1,2}\.\d+(-(alpha|beta|rc)(\.\d+)?)?$/i.test(release.tag_name);
  }

  constructor(release = null, version = null) {
    if (release) {
      this.release = release;
      this.version = release.tag_name.replace(/^host-/, '');
      this.versionName = release.name;
      this.versionTag = release.tag_name;
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
    // A pre-release ranks below the final release of the same version
    this.preRank = Infinity;
    const pre = typeof this.version === 'string' ? /-(alpha|beta|rc)(?:\.(\d+))?/i.exec(this.version) : null;
    if (pre) {
      const kind = { alpha: 0, beta: 1, rc: 2 }[pre[1].toLowerCase()];
      this.preRank = kind * 65536 + (pre[2] ? parseInt(pre[2], 10) : 0);
    }
  }

  /**
   * Parse `[host-][v]YYYY.MM.DD[.BUILD][-beta.N]` into [year, month, day, build]. Anything after the
   * build number (a commit hash or `dirty` marker of a local build) is ignored. Returns null when unusable.
   */
  parseVersion(version) {
    if (typeof version !== 'string') {
      return null;
    }
    const match = /^(?:host-)?v?(\d{1,4})\.(\d{1,2})\.(\d{1,2})(?:\.(\d+))?(?![\d])/i.exec(version.trim());
    if (!match) {
      return null;
    }
    return [match[1], match[2], match[3], match[4] ?? "0"].map(i => parseInt(i, 10));
  }

  isGreater(otherVersion) {
    let other;
    if (otherVersion instanceof NyxPlayVersion) {
      other = otherVersion;
    } else if (typeof otherVersion === 'string') {
      other = new NyxPlayVersion(null, otherVersion);
    } else {
      throw new Error('Invalid argument: otherVersion must be a NyxPlayVersion object or a version string');
    }

    if (!this.versionParts || !other.versionParts) {
      return false;
    }
    for (let i = 0; i < 4; i++) {
      if (this.versionParts[i] > other.versionParts[i]) {
        return true;
      } else if (this.versionParts[i] < other.versionParts[i]) {
        return false;
      }
    }
    return this.preRank > other.preRank;
  }
}

export default NyxPlayVersion;
