"""Release-gate tests for the signed WinSparkle appcast generator."""

from __future__ import annotations

import base64
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from nacl.signing import SigningKey


SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
SCRIPT = Path (__file__).with_name ("appcast.py")


class AppcastTest (unittest.TestCase):
   def run_generator (
      self,
      directory: Path,
      key: SigningKey,
      public_key: str,
      url: str = "https://releases.darkerdb.com/grimvault/2.0.0/GrimVault-Setup.exe",
   ) -> subprocess.CompletedProcess[str]:
      installer = directory / "GrimVault-Setup-prod-2.0.0.exe"
      installer.write_bytes (b"signed-installer-fixture")
      output = directory / "appcast.xml"
      env = os.environ.copy ()
      env ["APPCAST_ED25519_PRIV"] = base64.b64encode (bytes (key)).decode ("ascii")

      return subprocess.run (
         [
            sys.executable,
            str (SCRIPT),
            "--channel", "stable",
            "--existing", str (directory / "missing.xml"),
            "--installer", str (installer),
            "--output", str (output),
            "--public-key", public_key,
            "--url", url,
            "--version", "2.0.0",
         ],
         capture_output=True,
         check=False,
         env=env,
         text=True,
      )

   def test_generates_a_verifiable_enclosure_signature (self) -> None:
      with tempfile.TemporaryDirectory () as raw_directory:
         directory = Path (raw_directory)
         key = SigningKey.generate ()
         public_key = base64.b64encode (bytes (key.verify_key)).decode ("ascii")

         result = self.run_generator (directory, key, public_key)
         self.assertEqual (result.returncode, 0, result.stderr)

         root = ET.parse (directory / "appcast.xml").getroot ()
         enclosure = root.find ("channel/item/enclosure")
         self.assertIsNotNone (enclosure)
         assert enclosure is not None
         signature = base64.b64decode (enclosure.attrib [f"{{{SPARKLE}}}edSignature"])
         key.verify_key.verify (b"signed-installer-fixture", signature)

   def test_rejects_a_mismatched_public_key (self) -> None:
      with tempfile.TemporaryDirectory () as raw_directory:
         result = self.run_generator (
            Path (raw_directory),
            SigningKey.generate (),
            base64.b64encode (bytes (SigningKey.generate ().verify_key)).decode ("ascii"),
         )

         self.assertNotEqual (result.returncode, 0)
         self.assertIn ("does not match", result.stderr)

   def test_rejects_the_platform_release_host (self) -> None:
      with tempfile.TemporaryDirectory () as raw_directory:
         key = SigningKey.generate ()
         result = self.run_generator (
            Path (raw_directory),
            key,
            base64.b64encode (bytes (key.verify_key)).decode ("ascii"),
            "https://releases.katforge.com/grimvault/2.0.0/GrimVault-Setup.exe",
         )

         self.assertNotEqual (result.returncode, 0)
         self.assertIn ("releases.darkerdb.com/grimvault", result.stderr)


if __name__ == "__main__":
   unittest.main ()
