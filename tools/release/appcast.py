"""Generate a fail-closed WinSparkle appcast with an Ed25519 enclosure signature."""

from __future__ import annotations

import argparse
import base64
import binascii
import os
from email.utils import formatdate
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET
from urllib.parse import urlparse

from nacl.signing import SigningKey


SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
RELEASE_HOST = "releases.darkerdb.com"
VERSION = re.compile (
   r"^\d+\.\d+\.\d+(?:-(?:alpha|beta|rc)(?:[.-]\d+)?)?(?:\+[0-9A-Za-z.-]+)?$"
)


def args () -> argparse.Namespace:
   parser = argparse.ArgumentParser ()
   parser.add_argument ("--channel", choices=("beta", "stable"), required=True)
   parser.add_argument ("--existing", type=Path, required=True)
   parser.add_argument ("--installer", type=Path, required=True)
   parser.add_argument ("--output", type=Path, required=True)
   parser.add_argument ("--public-key", required=True)
   parser.add_argument ("--url", required=True)
   parser.add_argument ("--version", required=True)
   return parser.parse_args ()


def signing_key (expected_public: str) -> SigningKey:
   encoded = os.environ.get ("APPCAST_ED25519_PRIV", "")
   if not encoded:
      raise ValueError ("APPCAST_ED25519_PRIV is required")

   try:
      raw = base64.b64decode (encoded, validate=True)
   except binascii.Error as error:
      raise ValueError ("APPCAST_ED25519_PRIV is not valid base64") from error

   if len (raw) == 64:
      raw = raw[:32]
   if len (raw) != 32:
      raise ValueError ("APPCAST_ED25519_PRIV must decode to a 32-byte seed or 64-byte key")

   key = SigningKey (raw)
   actual = base64.b64encode (bytes (key.verify_key)).decode ("ascii")
   if actual != expected_public:
      raise ValueError ("appcast private key does not match the public key embedded in GrimVault")
   return key


def document (path: Path) -> tuple[ET.ElementTree, ET.Element]:
   if path.exists ():
      tree = ET.parse (path)
      root = tree.getroot ()
      channel = root.find ("channel")
      if root.tag != "rss" or channel is None:
         raise ValueError ("existing appcast is not an RSS channel")
      return tree, channel

   root = ET.Element ("rss", { "version": "2.0" })
   channel = ET.SubElement (root, "channel")
   ET.SubElement (channel, "title").text = "GrimVault Updates"
   ET.SubElement (channel, "link").text = "https://darkerdb.com/grimvault"
   ET.SubElement (channel, "description").text = "Signed GrimVault desktop releases"
   return ET.ElementTree (root), channel


def main () -> int:
   options = args ()
   if not VERSION.fullmatch (options.version):
      raise ValueError (f"invalid release version: {options.version}")
   if not options.installer.is_file ():
      raise ValueError (f"installer does not exist: {options.installer}")
   installer_url = urlparse (options.url)
   if (
      installer_url.scheme != "https"
      or installer_url.netloc != RELEASE_HOST
      or not installer_url.path.startswith ("/grimvault/")
   ):
      raise ValueError ("installer URL must use releases.darkerdb.com/grimvault")

   key = signing_key (options.public_key)
   signature = key.sign (options.installer.read_bytes ()).signature
   encoded_signature = base64.b64encode (signature).decode ("ascii")

   ET.register_namespace ("sparkle", SPARKLE)
   tree, channel = document (options.existing)

   for item in list (channel.findall ("item")):
      old_version = item.findtext (f"{{{SPARKLE}}}version")
      if old_version == options.version:
         channel.remove (item)

   item = ET.Element ("item")
   ET.SubElement (item, "title").text = f"GrimVault {options.version}"
   ET.SubElement (item, f"{{{SPARKLE}}}version").text = options.version
   ET.SubElement (item, f"{{{SPARKLE}}}shortVersionString").text = options.version
   ET.SubElement (item, f"{{{SPARKLE}}}minimumSystemVersion").text = "10.0.17763"
   ET.SubElement (item, "pubDate").text = formatdate (usegmt=True)
   ET.SubElement (item, "enclosure", {
      "url": options.url,
      "length": str (options.installer.stat ().st_size),
      "type": "application/vnd.microsoft.portable-executable",
      f"{{{SPARKLE}}}edSignature": encoded_signature,
      f"{{{SPARKLE}}}version": options.version,
   })

   first_item = next ((i for i, child in enumerate (channel) if child.tag == "item"), len (channel))
   channel.insert (first_item, item)
   for stale in channel.findall ("item")[20:]:
      channel.remove (stale)

   ET.indent (tree, space="   ")
   tree.write (options.output, encoding="utf-8", xml_declaration=True)
   print (f"wrote signed {options.channel} appcast: {options.output}")
   return 0


if __name__ == "__main__":
   try:
      sys.exit (main ())
   except (ET.ParseError, OSError, ValueError) as error:
      print (f"appcast: {error}", file=sys.stderr)
      sys.exit (1)
