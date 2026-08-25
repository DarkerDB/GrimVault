#pragma once

#include <gv/api/darkerdb_client.h>
#include <gv/core/result.h>

#include <functional>
#include <memory>

namespace gv::collection {

class Collector
{
public:
   using Sender = std::function<core::Result<api::CollectionResult> (const api::CollectionSample&)>;

   explicit Collector (api::DDBClient& client);
   explicit Collector (Sender sender);
   ~Collector ();

   Collector (const Collector&) = delete;
   Collector& operator= (const Collector&) = delete;

   void set_enabled (bool enabled);
   bool enabled () const noexcept;
   bool submit (api::CollectionSample sample);
   void stop ();

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

}
