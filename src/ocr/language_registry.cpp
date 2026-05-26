#include <gv/ocr/language_registry.h>
#include <gv/core/logger.h>

namespace gv::ocr {

LanguageRegistry::LanguageRegistry (std::filesystem::path models_root, std::size_t max_resident)
   : models_root_  (std::move (models_root)),
     max_resident_ (max_resident)
{}

core::Result<PaddleRecognizer*> LanguageRegistry::acquire (LanguageFamily family)
{
   std::lock_guard lock { lock_ };

   auto it = by_family_.find (family);

   if (it != by_family_.end ()) {
      order_.splice (order_.begin (), order_, it->second);
      return it->second->rec.get ();
   }

   const auto base    = models_root_ / "paddle" / std::string { family_dir (family) };
   const auto model   = base / "rec.onnx";
   const auto dictpth = base / "dict.txt";

   auto rec = std::make_unique<PaddleRecognizer> ();
   rec->set_family (family);

   auto r = rec->initialize (model, dictpth);

   if (!r.has_value ()) {
      return core::fail (r.error ());
   }

   while (order_.size () >= max_resident_) {
      evict_lru_locked ();
   }

   order_.push_front (Entry { .family = family, .rec = std::move (rec) });
   by_family_ [family] = order_.begin ();

   core::Logger::info ("language_registry: loaded {} (resident={})",
      family_dir (family), order_.size ());

   return order_.front ().rec.get ();
}

void LanguageRegistry::evict_lru_locked ()
{
   if (order_.empty ()) return;

   const auto victim = order_.back ().family;
   by_family_.erase (victim);
   order_.pop_back ();

   core::Logger::info ("language_registry: evicted {} (LRU)", family_dir (victim));
}

std::vector<LanguageFamily> LanguageRegistry::resident () const
{
   std::lock_guard lock { lock_ };
   std::vector<LanguageFamily> out;
   out.reserve (order_.size ());
   for (const auto& e : order_) out.push_back (e.family);
   return out;
}

} // namespace gv::ocr
