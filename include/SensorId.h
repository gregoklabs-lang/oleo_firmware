#pragma once

#include <Arduino.h>
#include <cstring>

inline String getShortDeviceId(const String &deviceId)
{
  String trimmed = deviceId;
  trimmed.trim();
  if (trimmed.isEmpty())
  {
    return trimmed;
  }

  String upper = trimmed;
  upper.toUpperCase();
  static const char *kPrefixes[] = {"LAB_", "OLEO_"};
  for (const char *prefix : kPrefixes)
  {
    const size_t len = strlen(prefix);
    if (upper.startsWith(prefix))
    {
      const String withoutPrefix = upper.substring(len);
      if (!withoutPrefix.isEmpty())
      {
        return withoutPrefix;
      }
    }
  }

  return upper;
}

inline String sanitizeSegment(const String &segment)
{
  String value = segment;
  value.trim();
  value.toUpperCase();
  value.replace(' ', '_');
  value.replace(':', '_');
  value.replace('.', '_');
  value.replace('/', '_');
  return value;
}

inline String makeSensorId(const String &deviceId, const String &bus, const String &address)
{
  String shortId = getShortDeviceId(deviceId);
  if (shortId.isEmpty())
  {
    shortId = deviceId;
    shortId.trim();
    shortId.toUpperCase();
    if (shortId.isEmpty())
    {
      shortId = "UNKNOWN";
    }
  }

  const String busClean = sanitizeSegment(bus);
  const String addrClean = sanitizeSegment(address);
  return "SNR_" + shortId + "_" + busClean + "_" + addrClean;
}
