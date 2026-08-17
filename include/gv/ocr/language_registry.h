#pragma once

#include <gv/core/result.h>
#include <gv/ocr/language.h>
#include <gv/ocr/paddle_recognizer.h>

#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace gv::ocr {

// Lazy-loaded pool of PaddleRecognizers, one per LanguageFamily. Keeps at
// most max_resident recognizers in memory; evicts the least-recently-used
// when a new family is requested past the limit.
//
// Used by the OCR pipeline: when the active game language changes, call
// `acquire(family)` to get an eviction-safe shared lease; if not resident, it is loaded
// from <models_root>/paddle/<family-dir>/, resolved by the names in
// gv::ocr::model_files (language.h).
class LanguageRegistry
{
public:
   LanguageRegistry (std::filesystem::path models_root, std::size_t max_resident = 2);

   using Lease = std::shared_ptr<PaddleRecognizer>;

   core::Result<Lease> acquire (LanguageFamily family);

   // For Diagnostics: list currently loaded families.
   std::vector<LanguageFamily> resident () const;

private:
   struct Entry {
      LanguageFamily                   family;
      Lease                              rec;
   };

   void evict_lru_locked ();

   std::filesystem::path                        models_root_;
   std::size_t                                  max_resident_;
   mutable std::mutex                           lock_;

   // LRU list: front = most recent, back = LRU.
   std::list<Entry>                             order_;
   std::unordered_map<LanguageFamily, typename std::list<Entry>::iterator> by_family_;
};

} // namespace gv::ocr
