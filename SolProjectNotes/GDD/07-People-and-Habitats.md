# 07 — People and Habitats

## Intent

People are individuals on ships, habitats, stations, and colonies—not only an anonymous workforce number. Their presence should make vehicle and infrastructure design matter through capacity, safety, roles, endurance, and consequence.

Initial scale targets are:

- ships: up to 32 people;
- stations: up to 128 people;
- colonies: up to 256 people.

These are starting design targets and require performance validation.

## Individual model

Potential persistent attributes include identity, employer/faction, role and skills, assignments, health, exposure, morale, relationships, experience, and needs. The first crew implementation should include only attributes that create decisions in construction, mission planning, or operations.

Individual does not require every person to run a full per-frame AI. On-duty local actions, scheduled routines, and background aggregate updates can coexist while preserving identity and consequential state.

## Designed habitats

The player designs ships, habitats, and stations. Construction should eventually account for pressure/atmosphere, power, heat, radiation protection, life support, workspaces, storage, access/egress, emergency capability, and comfort at the selected fidelity.

Whether players lay out walkable interiors, use functional modules, or combine both is open and has major engine/content consequences.

## Colonies

Colonies are late-game outcomes of logistics and habitation capability. They may develop identities and become politically independent. The player’s control model—direct management, company governance, contracts/influence, or a hybrid—remains open.

## Decisions

| Decision | Status | Why |
|---|---|---|
| People retain individual identity | Confirmed | User specified individual people |
| Ships/stations/colonies are player-designed | Confirmed | User response |
| Individual identity with simulation LOD | Proposed | Preserves meaning within long-duration performance budgets |
| Walkable interiors and daily-life depth | Open | Large scope and architecture consequence |
