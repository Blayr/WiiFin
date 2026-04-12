#pragma once
#include <string>

/* One saved Jellyfin account.
 * The password is NEVER stored — only the access token returned by the server. */
struct SavedProfile {
    std::string serverUrl;
    std::string username;    /* display name; empty when authenticated via Quick Connect */
    std::string serverName;  /* friendly name retrieved from the server */
    std::string userId;
    std::string accessToken; /* Jellyfin session token — not the password */
};
