/*
  ============================================================
   core/portal_provision.h  -- app-free provisioning via a web portal
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  There is no phone app yet, but we still need to commission 200 devices
  for the 50-house pre-production run, from a SINGLE firmware binary. So an
  uncommissioned device turns itself into a Wi-Fi hotspot with a small web
  form. The installer opens it from a phone browser, picks the role and
  fills in a few fields, and the DEVICE enrols itself against the Fleet API
  over HTTPS (real per-device certificate, real house key) -- exactly what
  the app would have done, minus the app.

  This is an ADDITIONAL method. BLE app provisioning is untouched and still
  reachable by holding the button at boot (see main.cpp).

  HOW THE INSTALLER USES IT (see README)
  --------------------------------------
    1. Power the box -> it makes a hotspot "ACHAS-PROV-<ID>", where <ID> is
       printed on the box label, so you always know which box you joined.
    2. Connect the phone to that hotspot; the form pops up.
    3. "Identify" button blinks the box's LED to confirm the right unit.
    4. GATEWAY: enter a house label + Wi-Fi + API token -> submit. The box
       creates the house, signs its certificate, stores the house key, and
       shows the HOUSE ID to write on that house's sensor boxes.
    5. SENSOR: enter that house id + box number + Wi-Fi + API token.

  SECURITY
  --------
   * The API token is typed by the installer per session and used only in
     RAM to call the Fleet API -- it is NEVER stored or baked in.
   * The gateway's private key is generated on-chip and never leaves it;
     only a CSR is sent. House key + cert arrive over HTTPS.

  WHERE IT IS USED
  ----------------
  main.cpp calls portal_provision_run() on an uncommissioned boot (after
  the self-test). On success the device reboots into its role. On timeout
  it returns so the caller can retry/sleep.
*/

#pragma once

// Run the provisioning portal. Blocks until the device is provisioned
// (then it reboots and never returns) or the window times out (then it
// returns so main.cpp can retry). See PORTAL_TIMEOUT_MS in config.h.
void portal_provision_run();
