#include <gv/core/settings.h>
#include <gv/db/repos/user_settings_repo.h>

#include <algorithm>
#include <charconv>
#include <sstream>

namespace gv::core {

namespace {

   constexpr std::string_view k_default_telemetry          = "true";
   constexpr std::string_view k_default_auto_updates       = "true";
   constexpr std::string_view k_default_launch_on_startup  = "true";
   constexpr std::string_view k_default_mode               = "automatic";
   constexpr std::string_view k_default_alignment          = "attached";
   constexpr std::string_view k_default_components         = "header,primary,secondary,details,quests,pricing";
   constexpr std::string_view k_default_scale              = "1.0";

   bool parse_bool (std::string_view s)
   {
      return s == "true" || s == "1" || s == "yes" || s == "on";
   }

   std::string fetch_or (
      gv::db::UserSettingsRepo& repo,
      std::string_view          key,
      std::string_view          fallback
   ) {
      auto r = repo.get (key);

      if (r.has_value () && r->has_value ()) {
         return **r;
      }

      return std::string { fallback };
   }

   std::vector<std::string> split_csv (std::string_view s)
   {
      std::vector<std::string> out;

      std::size_t start = 0;

      while (start < s.size ()) {
         auto end = s.find (',', start);

         if (end == std::string_view::npos) {
            end = s.size ();
         }

         auto part = s.substr (start, end - start);

         while (!part.empty () && (part.front () == ' ' || part.front () == '\t')) part.remove_prefix (1);
         while (!part.empty () && (part.back  () == ' ' || part.back  () == '\t')) part.remove_suffix (1);

         if (!part.empty ()) {
            out.emplace_back (part);
         }

         start = end + 1;
      }

      return out;
   }

   std::string join_csv (std::span<const std::string> v)
   {
      std::ostringstream ss;

      for (std::size_t i = 0; i < v.size (); ++i) {
         if (i) ss << ',';
         ss << v [i];
      }

      return ss.str ();
   }

} // namespace

Settings::Settings (gv::db::UserSettingsRepo& repo) : repo_ (repo) {}

bool      Settings::telemetry         () { return parse_bool (fetch_or (repo_, "general:telemetry",         k_default_telemetry)); }
bool      Settings::auto_updates      () { return parse_bool (fetch_or (repo_, "general:auto_updates",      k_default_auto_updates)); }
bool      Settings::launch_on_startup () { return parse_bool (fetch_or (repo_, "general:launch_on_startup", k_default_launch_on_startup)); }

Mode      Settings::default_mode      ()
{
   auto raw = fetch_or (repo_, "general:default_mode", k_default_mode);
   auto m   = parse_mode (raw);
   return m.value_or (Mode::Automatic);
}

Alignment Settings::alignment ()
{
   auto raw = fetch_or (repo_, "general:alignment", k_default_alignment);
   auto a   = parse_alignment (raw);
   return a.value_or (Alignment::Attached);
}

std::vector<std::string> Settings::components ()
{
   return split_csv (fetch_or (repo_, "general:components", k_default_components));
}

double Settings::scale ()
{
   auto raw = fetch_or (repo_, "general:scale", k_default_scale);
   double v = 1.0;
   std::from_chars (raw.data (), raw.data () + raw.size (), v);
   return v;
}

Result<void> Settings::set_telemetry          (bool v)       { return repo_.set ("general:telemetry",         v ? "true" : "false"); }
Result<void> Settings::set_auto_updates       (bool v)       { return repo_.set ("general:auto_updates",      v ? "true" : "false"); }
Result<void> Settings::set_launch_on_startup  (bool v)       { return repo_.set ("general:launch_on_startup", v ? "true" : "false"); }
Result<void> Settings::set_default_mode       (Mode v)       { return repo_.set ("general:default_mode",      to_string (v)); }
Result<void> Settings::set_alignment          (Alignment v)  { return repo_.set ("general:alignment",         to_string (v)); }
Result<void> Settings::set_components         (std::span<const std::string> v)
{
   return repo_.set ("general:components", join_csv (v));
}
Result<void> Settings::set_scale (double v)
{
   char buf [32];
   auto [p, ec] = std::to_chars (buf, buf + sizeof (buf), v);
   return repo_.set ("general:scale", std::string_view { buf, static_cast<std::size_t> (p - buf) });
}

std::string_view Settings::to_string (Mode m)
{
   switch (m) {
      case Mode::Automatic: return "automatic";
      case Mode::Manual:    return "manual";
      case Mode::Disabled:  return "disabled";
   }
   return "automatic";
}

std::string_view Settings::to_string (Alignment a)
{
   switch (a) {
      case Alignment::Attached:    return "attached";
      case Alignment::TopLeft:     return "top-left";
      case Alignment::TopRight:    return "top-right";
      case Alignment::BottomLeft:  return "bottom-left";
      case Alignment::BottomRight: return "bottom-right";
   }
   return "attached";
}

Result<Mode> Settings::parse_mode (std::string_view s)
{
   if (s == "automatic") return Mode::Automatic;
   if (s == "manual")    return Mode::Manual;
   if (s == "disabled")  return Mode::Disabled;

   return fail (Error::make (ErrorKind::InvalidArgument, "Unknown mode '{}'", s));
}

Result<Alignment> Settings::parse_alignment (std::string_view s)
{
   if (s == "attached")     return Alignment::Attached;
   if (s == "top-left")     return Alignment::TopLeft;
   if (s == "top-right")    return Alignment::TopRight;
   if (s == "bottom-left")  return Alignment::BottomLeft;
   if (s == "bottom-right") return Alignment::BottomRight;

   return fail (Error::make (ErrorKind::InvalidArgument, "Unknown alignment '{}'", s));
}

} // namespace gv::core
