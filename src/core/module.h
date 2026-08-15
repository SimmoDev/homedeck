#pragma once

namespace homedeck {

// The lifecycle contract every module (docs/architecture/modules.md,
// ADR-0003) implements - deliberately minimal, sized to what
// HarmonyConnection (core/harmony_connection.h, the reference
// implementation) actually needs, not designed speculatively ahead of a
// second module.
//
// Init is the constructor (wiring only - Storage/EventBus/HttpClient
// references, no I/O yet); Start()/Stop() bracket the module's actual
// background activity, the same two-phase shape AppCore itself already
// establishes project-wide (construct, then an explicit Start()). No
// separate "Teardown" step - the destructor is teardown, the same RAII
// contract Task already gives every module's own background work.
//
// A module being "enabled" is Core constructing and Start()-ing an
// instance of it; "disabled" is simply not doing so - see ADR-0003's
// per-module-type instance list decision (docs/decisions/ADR-0003-module-architecture.md).
// AppCore holds exactly one HarmonyConnection today (the same
// single-member shape every other Core service already has - e.g.
// OpenMeteoWeatherProvider), which generalizes to a list without
// redesign once a second concurrent instance of the same module type is
// ever needed - not built now, since nothing needs it yet.
class Module {
public:
    virtual ~Module() = default;

    virtual void Start() = 0;
    virtual void Stop() = 0;
};

}  // namespace homedeck
