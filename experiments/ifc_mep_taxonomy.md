# IFC MEP Equipment Taxonomy

Extracted from the IFC 4.3.2 (ISO 16739) open standard maintained by
buildingSMART International. This is the BIM-native classification of
MEP equipment types, their predefined subtypes, and salient properties.

Source: https://ifc43-docs.standards.buildingsmart.org/

## Taxonomy Structure

IFC classifies MEP equipment through an inheritance hierarchy:

```
IfcProduct
  └─ IfcElement
       └─ IfcDistributionElement
            └─ IfcDistributionFlowElement
                 ├─ IfcEnergyConversionDevice    (boilers, chillers, heat exchangers, coils)
                 ├─ IfcFlowController             (valves, dampers, switches)
                 ├─ IfcFlowFitting                (tees, elbows, reducers)
                 ├─ IfcFlowMovingDevice           (pumps, fans, compressors)
                 ├─ IfcFlowSegment                (pipes, ducts, cables)
                 ├─ IfcFlowStorageDevice          (tanks, vessels, batteries)
                 ├─ IfcFlowTerminal               (air terminals, fixtures, outlets)
                 └─ IfcFlowTreatmentDevice        (filters, silencers, separators)
```

Each concrete entity has a `PredefinedType` enumeration (the subtypes)
and one or more `Pset_*` property sets (the salient features).

---

## Energy Conversion Devices

Equipment that converts energy between forms.

### IfcBoiler
Converts fuel or electricity to heat via combustion or resistance.

**Predefined Types:**
- `WATER` — hot water boiler
- `STEAM` — steam boiler

**Salient Properties (Pset_BoilerTypeCommon):**
- PressureRating, OperatingPressure
- WaterStorageCapacity, EnergySource
- NominalPartLoadRatio, NominalEfficiency

### IfcChiller
Removes heat from a liquid via vapor-compression or absorption cycle.

**Predefined Types:**
- `AIRCOOLED` — rejects heat to air
- `WATERCOOLED` — rejects heat to condenser water
- `HEATRECOVERY` — simultaneously heats and cools

### IfcCoil
Heat transfer device in air or water systems (heating/cooling coils).

**Predefined Types:**
- `DXCOOLINGCOIL` — direct expansion refrigerant coil
- `ELECTRICHEATINGCOIL`
- `GASHEATINGCOIL`
- `HYDRONICCOIL` — hot or chilled water coil
- `STEAMHEATINGCOIL`
- `WATERCOOLINGCOIL`
- `WATERHEATINGCOIL`

### IfcHeatExchanger
Transfers thermal energy between two fluids without mixing.

**Predefined Types:**
- `PLATE` — plate-and-frame heat exchanger
- `SHELLANDTUBE` — shell-and-tube heat exchanger
- `TURNOUTHEATING` — turnout heating for rail applications

**Salient Properties (Pset_HeatExchangerTypeCommon):**
- FlowArrangement (counterflow, crossflow, parallelflow, multipass)

### IfcCompressor
Increases pressure of a gas (refrigerant compressor in heat pumps).

**Predefined Types:**
- `BOOSTER`, `DYNAMIC`, `HERMETIC`, `OPENTYPE`
- `RECIPROCATING`, `ROLLINGPISTON`, `ROTARY`
- `ROTARYVANE`, `SCROLL`, `SEMIHERMETIC`
- `SINGLESCREW`, `SINGLESTAGE`, `TROCHOIDAL`
- `TWINSCREW`, `WELDEDSHELLHERMETIC`

### IfcCondenser
Heat rejection side of a refrigeration cycle.

**Predefined Types:**
- `AIRCOOLED`, `EVAPORATIVECOOLED`, `WATERCOOLED`
- `WATERCOOLEDBRAZEDPLATE`, `WATERCOOLEDSHELLCOIL`
- `WATERCOOLEDSHELLCOILTUBE`, `WATERCOOLEDSHELLTUBE`

### IfcEvaporator
Heat absorption side of a refrigeration cycle.

**Predefined Types:**
- `DIRECTEXPANSION`, `DIRECTEXPANSIONBRAZEDPLATE`
- `DIRECTEXPANSIONSHELLANDTUBE`, `DIRECTEXPANSIONTUBEINTUBE`
- `FLOODEDSHELLANDTUBE`, `SHELLANDCOIL`

### IfcCoolingTower
Rejects heat to atmosphere via evaporative cooling.

**Predefined Types:**
- `MECHANICALFORCEDDRAFT` — fan forces air through
- `MECHANICALINDUCEDDRAFT` — fan draws air through
- `NATURALDRAFT` — buoyancy-driven airflow

### IfcUnitaryEquipment
Self-contained HVAC unit (packaged systems).

**Predefined Types:**
- `AIRCONDITIONINGUNIT`, `AIRHANDLER`
- `DEHUMIDIFIER`, `ROOFTOPUNIT`, `SPLITSYSTEM`

### IfcBurner
Combustion device within a boiler or furnace.

**Predefined Types:**
- `USERDEFINED`, `NOTDEFINED`

### IfcEvaporativeCooler
Cools air using water evaporation (swamp cooler).

**Predefined Types:**
- `DIRECTEVAPORATIVEAIRWASHER`, `DIRECTEVAPORATIVEPACKAGEDROTARYAIRCOOLER`
- `DIRECTEVAPORATIVERANDOMMEDIAAIRCOOLER`, `DIRECTEVAPORATIVERIGIDMEDIAAIRCOOLER`
- `DIRECTEVAPORATIVESLINGERSPACKAGEDAIRCOOLER`
- `INDIRECTDIRECTCOMBINATION`, `INDIRECTEVAPORATIVECOOLINGTOWERORCOILCOOLER`
- `INDIRECTEVAPORATIVEPACKAGEAIRCOOLER`, `INDIRECTEVAPORATIVEWETCOIL`

### IfcCooledBeam
Hydronic cooling terminal using convection/radiation.

**Predefined Types:**
- `ACTIVE` — with forced air supply
- `PASSIVE` — convection only

### IfcAirToAirHeatRecovery
Recovers energy from exhaust air to condition supply air.

**Predefined Types:**
- `FIXEDPLATECOUNTERFLOWEXCHANGER`, `FIXEDPLATECROSSFLOWEXCHANGER`
- `FIXEDPLATEPARALLELFLOWEXCHANGER`, `HEATPIPE`
- `ROTARYWHEEL`, `RUNAROUNDCOILLOOP`, `THERMOSIPHONCOILTYPEHEATEXCHANGERS`
- `THERMOSIPHONSEALEDTUBEHEATEXCHANGERS`, `TWINTOWERENTHALPYRECOVERYLOOPS`

---

## Flow Controllers

Devices that regulate flow direction, rate, or pressure.

### IfcValve
Controls flow in piping systems.

**Predefined Types:**
- `AIRRELEASE` — releases trapped air from pipe
- `ANTIVACUUM` — admits air if pressure drops below atmospheric
- `CHANGEOVER` — switches flow between pipelines (3/4-port)
- `CHECK` — permits one-way flow only
- `COMMISSIONING` — for system commissioning
- `DIVERTING` — diverts flow between branches (3-port)
- `DOUBLECHECK` — backflow prevention assembly
- `DOUBLEREGULATING` — flow regulation
- `DRAWOFFCOCK` — fluid removal point
- `FAUCET` — flow discharge fixture
- `FLUSHING` — flushes predetermined water quantity
- `GASCOCK` — gas flow control
- `GASTAP` — gas venting/discharge
- `ISOLATING` — closes off flow
- `MIXING` — mixes flow from two branches (3-port)
- `PRESSUREREDUCING` — reduces downstream pressure
- `PRESSURERELIEF` — auto-discharges at excessive pressure
- `REGULATING` — flow regulation
- `SAFETYCUTOFF` — closes via safety mechanism
- `STEAMTRAP` — restricts steam, passes condensate
- `STOPCOCK` — domestic water isolation

### IfcDamper
Controls airflow in duct systems.

**Predefined Types:**
- `BACKDRAFTDAMPER`, `BALANCINGDAMPER`, `BLASTDAMPER`
- `CONTROLDAMPER`, `FIREDAMPER`, `FIRESMOKEDAMPER`
- `FUMEHOODEXHAUST`, `GRAVITYDAMPER`, `GRAVITYRELIEFDAMPER`
- `RELIEFDAMPER`, `SMOKEDAMPER`

### IfcFlowMeter
Measures fluid or gas flow rate.

**Predefined Types:**
- `ENERGYMETER`, `GASMETER`, `OILMETER`, `WATERMETER`

---

## Flow Moving Devices

Create pressure differential to move fluids or gases.

### IfcPump
Circulates liquid through distribution systems.

**Predefined Types:**
- `CIRCULATOR` — inline circulation pump (e.g., Taco, Grundfos)
- `ENDSUCTION` — end-suction centrifugal pump
- `SPLITCASE` — horizontally split case pump
- `SUBMERSIBLEPUMP` — operates submerged
- `SUMPPUMP` — removes accumulated liquid
- `VERTICALINLINE` — inline vertical mount
- `VERTICALTURBINE` — deep well turbine pump

**Salient Properties (Pset_PumpTypeCommon):**
- FlowRateRange, FlowResistanceRange
- ConnectionSize, TemperatureRange
- NetPositiveSuctionHead, NominalRotationSpeed

### IfcFan
Moves air through distribution systems.

**Predefined Types:**
- `CENTRIFUGALAIRFOIL`, `CENTRIFUGALBACKWARDINCLINEDCURVED`
- `CENTRIFUGALFORWARDCURVED`, `CENTRIFUGALRADIAL`
- `PROPELLORAXIAL`, `TUBEAXIAL`, `VANEAXIAL`

### IfcCompressor
(See Energy Conversion Devices above)

---

## Flow Storage Devices

Store substances temporarily.

### IfcTank
Storage vessel for liquids (buffer tanks, expansion tanks, water heaters).

**Predefined Types:**
- `BASIN` — open top vessel
- `BREAKPRESSURE` — maintains head pressure
- `EXPANSION` — accommodates thermal expansion
- `FEEDANDEXPANSION` — combined feed and expansion
- `OILRETENTIONTRAY` — oil spill containment
- `PRESSUREVESSEL` — operates above atmospheric pressure
- `STORAGE` — general liquid storage
- `VESSEL` — general vessel

---

## Flow Terminals

Beginning or end points of distribution systems.

### IfcAirTerminal
Supply, return, or exhaust air opening.

**Predefined Types:**
- `DIFFUSER`, `GRILLE`, `LOUVRE`, `REGISTER`

### IfcAirTerminalBox
Regulates air volume at terminal point.

**Predefined Types:**
- `CONSTANTFLOW`, `VARIABLEFLOWPRESSUREDEPENDANT`
- `VARIABLEFLOWPRESSUREINDEPENDANT`

### IfcSpaceHeater
Terminal heating device in occupied space.

**Predefined Types:**
- `CONVECTOR` — convective heater
- `RADIATOR` — radiant heater

---

## Flow Treatment Devices

Alter properties of the transported medium.

### IfcFilter
Removes contaminants from air or water.

**Predefined Types:**
- `AIRPARTICLEFILTER`, `COMPRESSEDAIRFILTER`
- `ODORFILTER`, `OILFILTER`, `STRAINER`, `WATERFILTER`

### IfcDuctSilencer
Attenuates noise in ductwork.

---

## Flow Fittings

Connect segments of distribution systems.

### IfcPipeFitting
Connection component in piping systems.

**Predefined Types:**
- `BEND` — change of direction
- `CONNECTOR` — joins two segments
- `ENTRY` — system entry point
- `EXIT` — system exit point
- `JUNCTION` — branch connection (tee, wye)
- `OBSTRUCTION` — flow restriction
- `TRANSITION` — size change (reducer)

### IfcDuctFitting
Connection component in ductwork.

**Predefined Types:**
- `BEND`, `CONNECTOR`, `ENTRY`, `EXIT`
- `JUNCTION`, `OBSTRUCTION`, `TRANSITION`

---

## Cross-Reference: IFC Types to Real Equipment

| IFC Entity | Predefined Type | Real-World Equipment | Corpus Examples |
|---|---|---|---|
| IfcPump | CIRCULATOR | Taco 0013, Grundfos MAGNA3 | GRUNDFOS |
| IfcHeatExchanger | PLATE | Caleffi, flat plate HX | CALEFFI |
| IfcFlowTreatmentDevice | — | Air separators, dirt separators | CALEFFI/551006A |
| IfcTank | EXPANSION | Expansion tanks | JS-30-063 DX |
| IfcTank | STORAGE | Buffer tanks, water heaters | HF-22-BT |
| IfcUnitaryEquipment | SPLITSYSTEM | Mini-split heat pumps | MSZ-FS12NA |
| IfcCompressor | SCROLL | Heat pump compressor | QAHV-N136YAU |
| IfcCoil | HYDRONICCOIL | Fan coil units | — |
| IfcBoiler | WATER | Hot water boilers | — |
| IfcChiller | AIRCOOLED | Air-cooled chillers | — |
| IfcValve | MIXING | Thermostatic mixing valves | TEKMAR |
| IfcValve | CHECK | Check valves | — |
| IfcValve | ISOLATING | Ball/butterfly valves | — |
| IfcValve | PRESSURERELIEF | PRV, T&P relief valves | — |
| IfcCoolingTower | MECHANICALINDUCEDDRAFT | Cooling towers | — |
| IfcCondenser | WATERCOOLED | Water-source heat pump condenser | COLMAC/CXW-15 |
| IfcEvaporator | DIRECTEXPANSION | DX coil in mini-split | AERMEC/NYG0500-G |

---

## Notes on Heat Pumps in IFC

IFC does not have a dedicated `IfcHeatPump` entity. Heat pumps are modeled as
assemblies of their component parts:
- `IfcCompressor` (SCROLL or ROTARY)
- `IfcCondenser` (AIRCOOLED or WATERCOOLED)
- `IfcEvaporator` (DIRECTEXPANSION)
- `IfcUnitaryEquipment` (SPLITSYSTEM) for packaged units

This reflects the MEP engineering view where a "heat pump" is a system composed
of components, not a single device. The MasterFormat code 23 81 40 (Heat Pumps
& Geothermal) maps to `IfcUnitaryEquipment.SPLITSYSTEM` at the system level.

## Relationship to Other Taxonomies

| IFC Entity | MasterFormat Section | OmniClass 23 | AHRI Program |
|---|---|---|---|
| IfcBoiler | 23 52 00 | 23-33 21 00 | BTS (Boilers) |
| IfcChiller | 23 64 00 | 23-33 23 00 | WCCL (Chillers) |
| IfcPump | 23 21 20 | 23-33 11 00 | — |
| IfcValve | 23 05 10 | 23-31 11 00 | — |
| IfcCoolingTower | 23 65 00 | 23-33 25 00 | CTI (Towers) |
| IfcUnitaryEquipment | 23 81 00 | 23-33 27 00 | AHSP (Heat Pumps) |
| IfcAirTerminal | 23 37 00 | 23-33 35 00 | — |
| IfcTank | 23 21 40 | 23-31 33 00 | RWH (Water Heaters) |
| IfcHeatExchanger | 23 57 00 | 23-33 19 00 | — |
| IfcFilter | 23 40 00 | 23-33 31 00 | — |
