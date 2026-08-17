#include <gv/ocr/language_registry.h>
#include <gv/core/logger.h>

namespace gv::ocr {

LanguageRegistry::LanguageRegistry (std::filesystem::path models_root, std::size_t max_resident)
   : models_root_  (std::move (models_root)),
     max_resident_ (max_resident)
{}

core::Result<LanguageRegistry::Lease> LanguageRegistry::acquire (LanguageFamily family)
{
   std::lock_guard lock { lock_ };

   auto it = by_family_.find (family);

   if (it != by_family_.end ()) {
      order_.splice (order_.begin (), order_, it->second);
      return it->second->rec;
   }

   const auto base = models_root_ / "paddle" / std::string { family_dir (family) };

   // English prefers our tooltip-trained recognizer and falls back to stock
   // PaddleOCR; every other family only has stock. The wide 48x960 line is
   // the English geometry either way.
   const bool tooltip_model = family == LanguageFamily::English
      && std::filesystem::exists (base / model_files::rec_tooltip_body)
      && std::filesystem::exists (base / model_files::rec_tooltip_dict);

   const auto stock = family == LanguageFamily::English
      ? std::pair { model_files::rec_ppocr_wide, model_files::rec_ppocr_wide_dict }
      : std::pair { model_files::rec_ppocr_narrow, model_files::rec_ppocr_narrow_dict };

   const auto model   = base / (tooltip_model ? model_files::rec_tooltip_body : stock.first);
   const auto dictpth = base / (tooltip_model ? model_files::rec_tooltip_dict : stock.second);

   auto rec = std::make_shared<PaddleRecognizer> ();
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

   core::Logger::info ("language_registry: loaded {} model={} (resident={})",
      family_dir (family), model.filename ().string (), order_.size ());

   return order_.front ().rec;
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
