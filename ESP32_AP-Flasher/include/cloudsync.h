#pragma once

#include <Arduino.h>

#include "commstructs.h"

// Content mode for tags the cloud manages: cloudsync downloads the image and
// sets the config, drawNew converts it like a static image.
#define CONTENTMODE_CLOUD 30

// Pulls a per-AP manifest from the cloud and pushes tag check-ins to it.
// Configured by /cloud.json: {"url":"https://...","ap":"<ap id>","token":"<bearer>"}
void cloudsyncTask(void* parameter);

// Queue a tag check-in for the cloud. Called from processDataReq, never blocks.
void cloudsyncEvent(const uint8_t mac[8], const struct AvailDataReq& adr, uint32_t now);
