#pragma once

#include <Arduino.h>

#include "commstructs.h"

// Content mode for tags the cloud manages: cloudsync downloads the image and
// sets the config, drawNew converts it like a static image.
#define CONTENTMODE_CLOUD 30

// Pulls a per-AP manifest from the cloud and pushes tag check-ins to it.
// Configured by /cloud.json: {"url":"https://...","ap":"<ap id>","token":"<bearer>"}; without it the
// AP registers with the default cloud and waits to be claimed.
void cloudsyncTask(void* parameter);

// GET /cloud: {"state", "ap", "url", "claim_code" (while unclaimed), "last_contact" (unix time),
// "poll_s", "queued" (check-ins waiting), "dropped" (check-ins lost since boot)}
String cloudsyncStatus();

// Where to claim this AP: "{url}/aps?code=XXXX-XXXX" while it waits to be claimed, else empty
String cloudsyncClaimLink();

// Queue a tag check-in for the cloud. Called from processDataReq, never blocks.
void cloudsyncEvent(const uint8_t mac[8], const struct AvailDataReq& adr, uint32_t now);
