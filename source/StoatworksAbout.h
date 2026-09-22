/*
 * Stoatworks Labs - About window data for pilot.
 *
 * PROVISIONAL, and hand-written rather than generated. The fleet's copy of this
 * file is produced by stoatworks-backend/scripts/sync-about.py from the
 * website's projects.json, which is the one place these facts are written down
 * — but pilot is not on the website yet, so there is nothing to generate from.
 * This is the shape the generator produces, filled in by hand, and it will be
 * overwritten by the first real sync. See AGENTS.md.
 *
 * `guide` is deliberately EMPTY: no user guide exists, and the About block
 * leaves a link out rather than showing a button that opens a 404. graticule
 * shipped the same way.
 *
 * `version` here is a fallback. The build injects the real one and overrides it.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "pilot";
    inline constexpr auto slug = "pilot";
    inline constexpr auto hook = "A tape loader, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/pilot/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/pilot";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
