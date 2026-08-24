#pragma once
#include <string>
#include <vector>

struct JellyfinLibrary {
    std::string id;
    std::string name;
    std::string collectionType; // "movies", "tvshows", "music", "books", ...
};

struct JellyfinItem {
    std::string id;
    std::string name;
    std::string type; // "Movie", "Series", "MusicAlbum", ...
    int         year = 0;
    long long   playbackPositionTicks = 0;
    long long   runtimeTicks          = 0;
    std::string seriesName;    // Episode only
    std::string seriesId;      // Episode only — parent series ID for backdrop lookup
    int         seasonNumber  = 0;
    int         episodeNumber = 0;
};

struct JellyfinSeason {
    std::string id;
    std::string name;
    int         indexNumber = 0;
};

struct JellyfinEpisode {
    std::string id;
    std::string name;
    int         indexNumber  = 0; // episode number within season
    int         seasonNumber = 0;
    long long   playbackPositionTicks = 0;
};

struct JellyfinPerson {
    std::string name;
    std::string role; // "Actor", "Director", "Writer", ...
    std::string character;
};

struct MediaStream {
    int         index        = 0;
    std::string type;         // "Audio", "Subtitle", "Video"
    std::string displayTitle;
    std::string language;
    std::string codec;
};

struct JellyfinItemDetail {
    std::string id;
    std::string name;
    std::string overview;      // synopsis
    std::string officialRating;
    int         year                  = 0;
    long long   runtimeTicks          = 0; // RunTimeTicks (64-bit from Jellyfin API)
    long long   playbackPositionTicks = 0; // UserData.PlaybackPositionTicks
    std::vector<std::string>    genres;          // up to 4
    std::vector<JellyfinPerson> people;          // up to 6 (cast + director)
    std::vector<MediaStream>    audioStreams;    // audio tracks
    std::vector<MediaStream>    subtitleStreams; // subtitle tracks
};

struct JellyfinAudioItem {
    std::string id;
    std::string name;        // track title
    std::string artist;      // AlbumArtist
    std::string album;
    int         trackNumber      = 0;
    long long   runtimeTicks     = 0;
    long long   playbackPositionTicks = 0;
};

struct JellyfinAuth {
    std::string userId;
    std::string accessToken;
    std::string serverName;
};

/* Intro / credits segment timestamps (from Jellyfin Intro Skipper API) */
struct IntroInfo {
    bool  hasIntro       = false;
    float introStart     = 0.0f; /* seconds — start of intro segment   */
    float introEnd       = 0.0f; /* seconds — end of intro segment      */
    float showPromptAt   = 0.0f; /* when to show the Skip-Intro prompt  */
    float hidePromptAt   = 0.0f; /* when to hide the prompt             */
};

struct QuickConnectResult {
    std::string code;    // 6-char code shown to user
    std::string secret;  // used to poll / authenticate
    bool authenticated;  // true once user approved on another device
};

struct DiscoveredServer {
    std::string name;    // server display name
    std::string address; // full URL, e.g. "http://192.168.1.10:8096"
};

class JellyfinClient {
public:
    // Initialize networking (call once)
    bool initNetwork();

    // Authenticate with username + password
    // Returns true on success, fills out auth
    bool authenticate(const std::string& serverUrl,
                      const std::string& username,
                      const std::string& password,
                      JellyfinAuth& out);

    // Quick Connect: initiate a session, returns code+secret or empty on failure
    bool quickConnectInitiate(const std::string& serverUrl, QuickConnectResult& out);

    // Quick Connect: poll until approved (call repeatedly), returns true when done
    bool quickConnectCheck(const std::string& serverUrl,
                           const std::string& secret,
                           QuickConnectResult& out);

    // Quick Connect: exchange secret for token after approval
    bool quickConnectAuthenticate(const std::string& serverUrl,
                                  const std::string& secret,
                                  JellyfinAuth& out);

    // Discover Jellyfin servers on the LAN via UDP broadcast (port 7359)
    // Blocks for ~2 s; returns true if socket opened (even if no servers found)
    bool discoverServers(std::vector<DiscoveredServer>& out);

    bool isNetworkReady() const { return networkReady; }
    const std::string& lastError() const { return errMsg; }

    // Closes any open plain-HTTP keep-alive connection (see httpRequest's
    // connection-reuse logic) and drops the socket. Call before handing
    // control to wii_player_play() -- MPlayer needs a large contiguous heap
    // and every held resource counts (same reasoning as
    // LibraryView::releaseForPlayback() freeing textures/loader threads).
    // Safe to call when nothing is open. The connection reopens
    // transparently on the next request either way; this only affects
    // memory pressure during playback, not correctness.
    void closeKeepAliveConnection();

    // Fetch user views (libraries: movies, tvshows, music...)
    bool getLibraries(const std::string& serverUrl,
                      const JellyfinAuth& auth,
                      std::vector<JellyfinLibrary>& out);

    // Fetch items inside a library or folder (paginated)
    bool getItems(const std::string& serverUrl,
                  const JellyfinAuth& auth,
                  const std::string& parentId,
                  int startIndex, int limit,
                  std::vector<JellyfinItem>& out,
                  int& totalCount);

    // Fetch albums for a specific artist (uses AlbumArtistIds, more reliable than ParentId)
    bool getAlbumsByArtist(const std::string& serverUrl,
                            const JellyfinAuth& auth,
                            const std::string& artistId,
                            int startIndex, int limit,
                            std::vector<JellyfinItem>& out,
                            int& totalCount);

    // Fetch raw image bytes for an item's primary thumbnail. itemName is
    // optional and purely cosmetic -- used to label the on-screen debug log
    // ("Poster: <name>") since itemId alone (a GUID) isn't useful there.
    bool getItemImageBytes(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& itemId,
                           int maxWidth, int maxHeight,
                           std::string& outBytes,
                           const std::string& itemName = "");

    // Like getItemImageBytes, but reads only from the on-disk cache -- never
    // touches the network. Returns false on any cache miss (including a
    // disabled/empty cacheDir), leaving outBytes untouched. Lets callers that
    // want to skip async/network loading for already-cached images check
    // cheaply before falling back to getItemImageBytes.
    bool getCachedItemImageBytes(const std::string& itemId,
                                 int maxWidth, int maxHeight,
                                 std::string& outBytes);

    // Fetch the best landscape image for activity cards:
    //   Episode → Images/Thumb, fallback series Backdrop, then Primary
    //   Movie/Series → Images/Backdrop/0, fallback Primary
    bool getItemBackdropBytes(const std::string& serverUrl,
                              const JellyfinAuth& auth,
                              const JellyfinItem& item,
                              int maxWidth, int maxHeight,
                              std::string& outBytes);

    // Like getItemBackdropBytes, but walks the same Thumb -> series Backdrop
    // -> Primary fallback chain reading only from the on-disk cache -- never
    // touches the network. Returns true once the chain reaches a resolved
    // answer purely from cache (real bytes, or a confirmed-absent marker at
    // every tier up to and including Primary -- true with outBytes left
    // empty, meaning "no image, and we don't need the network to know
    // that"). Returns false the moment it hits an unresolved tier (no cache
    // entry at all yet, positive or negative), since a real fallback chain
    // can't be walked without querying the server there.
    bool getCachedItemBackdropBytes(const JellyfinItem& item,
                                    int maxWidth, int maxHeight,
                                    std::string& outBytes);

    // Fetch the server name from /System/Info (fills auth.serverName)
    bool getServerName(const std::string& serverUrl,
                       const JellyfinAuth& auth,
                       std::string& outName);

    // Fetch up to 3 in-progress ("Continue Watching") video items
    bool getContinueWatching(const std::string& serverUrl,
                             const JellyfinAuth& auth,
                             std::vector<JellyfinItem>& out);

    // Fetch up to 3 "Next Up" episodes (next in series to watch)
    bool getNextUp(const std::string& serverUrl,
                   const JellyfinAuth& auth,
                   std::vector<JellyfinItem>& out);

    // Fetch BoxSet collections from a movies library (paginated)
    bool getMovieCollections(const std::string& serverUrl,
                             const JellyfinAuth& auth,
                             const std::string& parentId,
                             int startIndex, int limit,
                             std::vector<JellyfinItem>& out,
                             int& totalCount);

    // Fetch favourite movies from a library (paginated)
    bool getFavoriteMovies(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& parentId,
                           int startIndex, int limit,
                           std::vector<JellyfinItem>& out,
                           int& totalCount);

    // Fetch favourite series from a TV library (paginated) -- same as
    // getFavoriteMovies but IncludeItemTypes=Series.
    bool getFavoriteSeries(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& parentId,
                           int startIndex, int limit,
                           std::vector<JellyfinItem>& out,
                           int& totalCount);

    // Fetch all favourites globally (Movie, Series, MusicAlbum) across all libraries (paginated)
    bool getGlobalFavorites(const std::string& serverUrl,
                            const JellyfinAuth& auth,
                            int startIndex, int limit,
                            std::vector<JellyfinItem>& out,
                            int& totalCount);

    // Fetch continue-watching movies (up to 4)
    bool getMovieContinueWatching(const std::string& serverUrl,
                                  const JellyfinAuth& auth,
                                  std::vector<JellyfinItem>& out);

    // Fetch recently added movies via /Users/{id}/Items/Latest
    bool getMoviesLatest(const std::string& serverUrl,
                         const JellyfinAuth& auth,
                         const std::string& parentId,
                         int limit,
                         std::vector<JellyfinItem>& out);

    // Fetch continue-watching TV episodes (up to 4)
    bool getTVContinueWatching(const std::string& serverUrl,
                               const JellyfinAuth& auth,
                               std::vector<JellyfinItem>& out);

    // Fetch recently added TV series via /Users/{id}/Items/Latest
    bool getTVSeriesLatest(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& parentId,
                           int limit,
                           std::vector<JellyfinItem>& out);

    // Fetch upcoming (unaired) episodes via /Shows/Upcoming
    bool getTVUpcoming(const std::string& serverUrl,
                       const JellyfinAuth& auth,
                       int limit,
                       std::vector<JellyfinItem>& out);

    // Fetch recently added music albums via /Users/{id}/Items/Latest
    bool getMusicLatest(const std::string& serverUrl,
                        const JellyfinAuth& auth,
                        const std::string& parentId,
                        int limit,
                        std::vector<JellyfinItem>& out);

    // Fetch all playlists (paginated)
    bool getPlaylists(const std::string& serverUrl,
                      const JellyfinAuth& auth,
                      int startIndex, int limit,
                      std::vector<JellyfinItem>& out,
                      int& totalCount);

    // Fetch audio items inside a playlist
    bool getPlaylistTracks(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& playlistId,
                           std::vector<JellyfinAudioItem>& out);

    // Fetch full item metadata (overview, genres, cast, runtime)
    bool getItemDetail(const std::string& serverUrl,
                       const JellyfinAuth& auth,
                       const std::string& itemId,
                       JellyfinItemDetail& out);

    // Fetch seasons for a TV series
    bool getSeasons(const std::string& serverUrl,
                    const JellyfinAuth& auth,
                    const std::string& seriesId,
                    std::vector<JellyfinSeason>& out);

    // Fetch episodes for a season
    bool getEpisodes(const std::string& serverUrl,
                     const JellyfinAuth& auth,
                     const std::string& seriesId,
                     const std::string& seasonId,
                     std::vector<JellyfinEpisode>& out);

    // Fetch intro/credits timestamps for an episode.
    // Tries the Intro Skipper plugin endpoint first; returns false (no error)
    // if the server responds 404 (plugin not installed or no data for item).
    bool getIntroTimestamps(const std::string& serverUrl,
                            const JellyfinAuth& auth,
                            const std::string& episodeId,
                            IntroInfo& out);

    // Fetch tracks (Audio items) for a MusicAlbum
    bool getAlbumTracks(const std::string& serverUrl,
                        const JellyfinAuth& auth,
                        const std::string& albumId,
                        std::vector<JellyfinAudioItem>& out);

    // Build a direct audio transcoding URL for an Audio item.
    // Returns the full URL; empty string on failure.
    // outPlaySessionId is filled for later reportPlaybackStopped calls.
    bool getAudioStreamUrl(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& itemId,
                           long long startTimeTicks,
                           std::string& outUrl,
                           std::string& outPlaySessionId);

    // Ask Jellyfin to transcode via POST /Items/{id}/PlaybackInfo.
    // EnableDirectPlay and EnableDirectStream are set to false in the JSON body
    // (not just as query params) so Jellyfin cannot ignore them.
    // subtitleStreamIndex = -1 means no subtitles.
    // outPlaySessionId is filled from the response (needed for progress reporting).
    // Returns false if the request fails; outUrl/outPlaySessionId untouched.
    bool getTranscodingUrl(const std::string& serverUrl,
                           const JellyfinAuth& auth,
                           const std::string& itemId,
                           const std::string& mediaSourceId,
                           int audioStreamIndex,
                           int subtitleStreamIndex,
                           long long startTimeTicks,
                           std::string& outUrl,
                           std::string& outPlaySessionId);

    // Playback reporting — call before and after wii_player_play()
    // positionTicks is ignored by reportPlaybackStart; pass 0.
    bool reportPlaybackStart(const std::string& serverUrl,
                             const JellyfinAuth& auth,
                             const std::string& itemId,
                             const std::string& mediaSourceId,
                             const std::string& playSessionId);

    bool reportPlaybackProgress(const std::string& serverUrl,
                                const JellyfinAuth& auth,
                                const std::string& itemId,
                                const std::string& mediaSourceId,
                                const std::string& playSessionId,
                                long long positionTicks,
                                bool isPaused);

    bool reportPlaybackStopped(const std::string& serverUrl,
                               const JellyfinAuth& auth,
                               const std::string& itemId,
                               const std::string& mediaSourceId,
                               const std::string& playSessionId,
                               long long positionTicks);

    // Tell the server to kill the active FFmpeg transcode for this session.
    // Must be called after reportPlaybackStopped to prevent zombie encodes.
    // DELETE /Videos/ActiveEncodings?DeviceId=wiifin-wii&PlaySessionId={id}
    bool deleteActiveEncoding(const std::string& serverUrl,
                              const JellyfinAuth& auth,
                              const std::string& playSessionId);

    // Search items across all libraries. `itemTypes` is a comma-separated
    // Jellyfin item-type filter (e.g. "Series,Movie" to restrict to those
    // types only) -- defaults to everything searchable.
    bool searchItems(const std::string& serverUrl,
                     const JellyfinAuth& auth,
                     const std::string& searchTerm,
                     int limit,
                     std::vector<JellyfinItem>& out,
                     const std::string& itemTypes =
                         "Movie,Series,Episode,MusicAlbum,Audio,MusicArtist,BoxSet,Playlist");

    bool sslVerify = true;   // true = verify certificate (set false to allow self-signed)

    // Bitrate: Auto scales continuously off measured throughput (see
    // networkKbps below); Fast/Medium/Slow are fixed manual overrides for
    // when you know better than the estimate (or just want a predictable
    // number). Ceiling (3000) is a safety margin under the ~4.2 Mbps
    // measured wired throughput on this project's own test hardware (an
    // otherwise-idle Wii, FTP bulk transfer) -- real playback leaves less
    // headroom than that since the same weak CPU is also decoding MPEG2 +
    // driving GX. Floor (700) matches this codebase's original hardcoded
    // value from before any of this per-session measurement existed.
    enum class BitrateMode { Auto = 0, Fast = 1, Medium = 2, Slow = 3 };
    BitrateMode bitrateMode = BitrateMode::Auto;

    static constexpr float NETWORK_BITRATE_SAFETY_MARGIN = 0.6f; // Auto: clamp(networkKbps * this, floor, ceiling)
    static constexpr float NETWORK_BITRATE_FLOOR_KBPS     = 700.0f;  // == Slow
    static constexpr float NETWORK_BITRATE_MEDIUM_KBPS    = 1500.0f;
    static constexpr float NETWORK_BITRATE_CEILING_KBPS   = 3000.0f; // == Fast

    // Resolves bitrateMode (+ networkKbps for Auto) to an actual transcode
    // target in kbps -- shared by getTranscodingUrl(), the playback-start
    // toast, and the Settings display so they can never disagree.
    float effectiveBitrateKbps() const {
        switch (bitrateMode) {
            case BitrateMode::Fast:   return NETWORK_BITRATE_CEILING_KBPS;
            case BitrateMode::Medium: return NETWORK_BITRATE_MEDIUM_KBPS;
            case BitrateMode::Slow:   return NETWORK_BITRATE_FLOOR_KBPS;
            default: {
                float t = networkKbps * NETWORK_BITRATE_SAFETY_MARGIN;
                if (t < NETWORK_BITRATE_FLOOR_KBPS)   t = NETWORK_BITRATE_FLOOR_KBPS;
                if (t > NETWORK_BITRATE_CEILING_KBPS) t = NETWORK_BITRATE_CEILING_KBPS;
                return t;
            }
        }
    }

    // Transcode resolution. Unlike wired/wireless (no libogc API exposes
    // that), the Wii *can* reliably tell us whether the connected cable is
    // progressive (component/HDMI adapter) or interlaced-only (composite/
    // S-Video) -- see VIDEO_GetCurrentTvMode()'s VI_PROGRESSIVE vs
    // VI_INTERLACE/VI_NON_INTERLACE. Composite's real resolvable detail
    // (interlacing + ~330 TVL analog bandwidth) is well under 848x480, so
    // decoding full detail there just burns CPU headroom on this weak
    // hardware for pixels the display can't actually show.
    enum class VideoResMode { Auto = 0, Full = 1, Reduced = 2 };
    VideoResMode videoResMode = VideoResMode::Auto;
    // Set once at startup from VIDEO_GetCurrentTvMode(); drives Auto's choice.
    bool isProgressiveVideo = true;

    // Resolves videoResMode (+ isProgressiveVideo for Auto) to an actual
    // transcode target: Full/progressive-Auto = 848x480 (this app's normal
    // ceiling); Reduced/interlaced-Auto = 480x480, a rough match for
    // composite's real resolvable detail.
    void getEffectiveResolution(int& w, int& h) const {
        bool full = (videoResMode == VideoResMode::Full) ||
                    (videoResMode == VideoResMode::Auto && isProgressiveVideo);
        w = full ? 848 : 480;
        h = 480;
    }

    // Live measured throughput (kbps), used by effectiveBitrateKbps()'s Auto
    // case above. Earlier attempts tried to label the connection "Wired" or
    // "Wireless" (first via MAC -- confirmed to return the same value
    // regardless of adapter on this hardware/IOS combo, so unusable as an
    // identity key; then via a single wired/wireless threshold on this same
    // number) but real Wi-Fi throughput on this hardware varies too much
    // session to session (and apparently minute to minute) for a binary
    // label to track it. Scaling directly off the number sidesteps needing
    // a label at all: a good moment gets a genuinely higher bitrate, a bad
    // moment gets throttled down, regardless of which physical link
    // produced it. Persisted across launches as an optimistic starting
    // guess (see App::loadSettings/saveSettings), then immediately refined
    // by this session's real traffic via recordThroughputSample().
    float networkKbps = 0.0f;

    // Feeds one real HTTP response's measured size/time into the running
    // throughput estimate -- piggybacks on real image fetches during normal
    // browsing instead of needing a dedicated synthetic speed test. Small
    // responses (JSON API calls) are ignored: fixed per-request overhead
    // (DNS + TCP/TLS handshake) would dominate the timing and make the
    // estimate meaningless.
    void recordThroughputSample(size_t bytes, unsigned ms);

    // Called right before requesting a transcode for a brand new playback
    // session (see LibraryView.cpp's Play / Resume-confirm sites), while the
    // loading spinner (and its on-screen debug log) is already up. No-op if
    // bitrateMode isn't Auto (a fixed Fast/Medium/Slow choice doesn't need a
    // measurement at all) or if we already have one to go on (this
    // session's real samples, or last session's persisted value) --
    // otherwise forces one real, deliberately-oversized, guaranteed-cache-
    // miss image fetch so recordThroughputSample() has something to measure
    // before the bitrate decision is made, for a completely fresh
    // install/profile with no history at all.
    void ensureNetworkSpeedKnown(const std::string& serverUrl, const JellyfinAuth& auth,
                                  const std::string& itemId);

    // Directory to cache fetched image bytes in, e.g. "sd:/apps/WiiFin/cache/"
    // (trailing slash). Empty = caching disabled (no writable device found).
    std::string cacheDir;

    // Deletes every file under cacheDir (cached poster/backdrop images).
    // Main-thread-safe, no network I/O. Returns the number of files removed,
    // or -1 if cacheDir is empty (caching disabled) or couldn't be opened.
    int clearImageCache();

private:
    bool networkReady = false;
    std::string errMsg;
    std::string localIp_;   // set by initNetwork, used by discoverServers
    std::string localMask_;

    // Fetch bytes for `url`, transparently caching under cacheDir + cacheKey.
    // Falls back to a plain httpRequest() when cacheDir is empty. Cache hits skip
    // the network entirely; cache misses fetch normally and best-effort save the
    // response for next time (write failures are silent/non-fatal).
    // label is what a cache-miss network fetch shows in the on-screen debug
    // log, e.g. "Poster: Inception" or "Backdrop: The Office".
    bool fetchImageCached(const std::string& cacheKey,
                          const std::string& url,
                          const std::string& authToken,
                          std::string& outBytes,
                          const std::string& label);

    // Disk-only half of fetchImageCached: reads cacheDir + cacheKey into
    // outBytes if present. No network I/O, no TTL (entries only ever clear
    // via Settings' "Clear Image Cache" action).
    bool tryReadCachedImage(const std::string& cacheKey, std::string& outBytes);

    // Low-level HTTP/HTTPS: auto-detects scheme, returns HTTP status code
    int httpRequest(const std::string& url,
                    const std::string& method,
                    const std::string& contentType,
                    const std::string& body,
                    const std::string& authToken,
                    std::string& responseBody);

    // TLS path — called by httpRequest when scheme is https://
    int httpsRequest(const std::string& host, int port,
                     const std::string& path,
                     const std::string& method,
                     const std::string& contentType,
                     const std::string& body,
                     const std::string& authToken,
                     std::string& responseBody);

    // Minimal JSON field extractor: finds first "key":"value" and returns value
    std::string jsonGetString(const std::string& json, const std::string& key);
    // For boolean fields
    bool jsonGetBool(const std::string& json, const std::string& key);
    // For integer fields
    int  jsonGetInt(const std::string& json, const std::string& key);
    long long jsonGetLongLong(const std::string& json, const std::string& key);

    // Parse host/port/path from URL; sets isHttps to true for https:// scheme
    bool parseUrl(const std::string& url,
                  std::string& host, int& port,
                  std::string& basePath, bool& isHttps);
};
