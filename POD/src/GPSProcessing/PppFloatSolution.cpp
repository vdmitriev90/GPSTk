#include "PppFloatSolution.h"

#include"SimpleFilter.hpp"
#include"LinearCombinations.hpp"
#include"LICSDetector.hpp"
#include"MWCSDetector.hpp"
#include"Decimate.hpp"
#include"BasicModel.hpp"
#include"NeillTropModel.hpp"
#include"PowerSum.hpp"
#include"ComputeTropModel.hpp"
#include"LinearCombinations.hpp"
#include"EclipsedSatFilter.hpp"
#include"GravitationalDelay.hpp"
#include"LICSDetector2.hpp"
#include"MWCSDetector.hpp"
#include"SatArcMarker.hpp"
#include"AntexReader.hpp"
#include"ComputeSatPCenter.hpp"
#include"CorrectObservables.hpp"
#include"ComputeWindUp.hpp"
#include"SolidTides.hpp"
#include"OceanLoading.hpp"
#include"PoleTides.hpp"

#include"MJD.hpp"
#include"IonexModel.hpp"

#include"TropoEquations.h"
#include"ClockBiasEquations.h"
#include"PositionEquations.h"
#include"InterSystemBias.h"
#include"InterFrequencyBiases.h"
#include"AmbiguitiesEquations.h"
#include"SNRCatcher.h"
#include"IonoEquations.h"
#include"IonoStochasticModel.h"
#include"KalmanSolverFB.h"

#include"WinUtils.h"

using namespace std;
using namespace gpstk;

namespace pod
{
    PppFloatSolution::PppFloatSolution(GnssDataStore_sptr data_ptr)
        :GnssSolution(data_ptr, 50.0)
    { }
    PppFloatSolution::PppFloatSolution(GnssDataStore_sptr data_ptr, double max_sigma)
        : GnssSolution(data_ptr, max_sigma)
    { }
    void  PppFloatSolution::process()
    {
        updateRequaredObs();

        BasicModel model;
        model.setDefaultEphemeris(data->SP3EphList);
        model.setDefaultObservable(codeL1);
        model.setMinElev(opts().maskEl);

        SimpleFilter CodeFilter(TypeIDSet{ codeL1,TypeID::P2,TypeID::L1,TypeID::L2 });
        SimpleFilter SNRFilter(TypeID::S1, confReader().getValueAsInt("SNRmask"), DBL_MAX);
        // Object to remove eclipsed satellites
        EclipsedSatFilter eclipsedSV;

        gnssRinex gRin;

        //Object to decimate data
        Decimate decimateData(confReader().getValueAsDouble("decimationInterval"),
            confReader().getValueAsDouble("decimationTolerance"),
            data->SP3EphList.getInitialTime());

#pragma region troposhere modeling objects

        //for rover
        NeillTropModel tropoRovPtr;
        ComputeTropModel computeTropoRover(tropoRovPtr);

#pragma endregion

        IonexModel ionoModel(nominalPos, data->ionexStore, TypeID::C1, false);

        // Object to compute gravitational delay effects
        GravitationalDelay grDelayRover;

#pragma region CS detectors

        // Objects to mark cycle slips
        // Checks LI cycle slips
        LICSDetector2  markCSLI2Rover;
               markCSLI2Rover.setSatThreshold(confReader().getValueAsDouble("LISatThreshold"));

        // Checks Merbourne-Wubbena cycle slips
        MWCSDetector   markCSMW2Rover;
              markCSMW2Rover.setMaxNumLambdas(confReader().getValueAsDouble("MWNLambdas"));

        // check sharp SNR drops 
        SNRCatcher snrCatcherL1Rover;

        // Object to keep track of satellite arcs
        SatArcMarker markArcRover(TypeID::CSL1, true, 31.0);

#pragma endregion

#pragma region prepare ANTEX reader

        string antxfile = data->genericFilesDirectory;
        antxfile += confReader().getValue("antexFile");

        AntexReader antexReader;
        antexReader.open(antxfile);

#pragma endregion

#pragma region correct observable

        CorrectObservables corrRover(data->SP3EphList);

        // Vector from monument to antenna ARP [UEN], in meters
        Triple offsetARP;
        int i = 0;
        for (auto &it : confReader().getListValueAsDouble("offsetARP", data->SiteRover))
            offsetARP[i++] = it;
        corrRover.setMonument(offsetARP);

        Antenna roverAnt(antexReader.getAntenna(confReader().getValue("antennaModel", data->SiteRover)));
        corrRover.setAntenna(roverAnt);
        corrRover.setUseAzimuth(confReader().getValueAsBoolean("useAzim", data->SiteRover));

#pragma endregion

        // Objects to compute tidal effects
        SolidTides solid;
        PoleTides pole;
        // Configure ocean loading model
        OceanLoading ocean;
        ocean.setFilename(data->genericFilesDirectory + confReader().getValue("oceanLoadingFile"));

        ComputeWindUp windupRover(data->SP3EphList, data->genericFilesDirectory + confReader().getValue("satDataFile"));

        ComputeSatPCenter svPcenterRover;
        svPcenterRover.setAntexReader(antexReader);

        ProcessLinear linearIonoFree;
        linearIonoFree.add(make_unique<PCCombimnation>());
        linearIonoFree.add(make_unique<LCCombimnation>());

        KalmanSolver solver(Equations);
        KalmanSolverFB solverFb(Equations);
        if (forwardBackwardCycles > 0)
        {
            solverFb.setCyclesNumber(forwardBackwardCycles);
            solverFb.setLimits(confReader().getListValueAsDouble("codeLimList"), confReader().getListValueAsDouble("phaseLimList"));
        }
        ofstream ostream;
        ostream.open(data->workingDir + "\\" + fileName(), ios::out);

        bool firstTime = true;
        //
        for (auto &obsFile : data->getObsFiles(data->SiteRover))
        {
            cout << obsFile << endl;
            //Input observation file stream
            Rinex3ObsStream rin;

            //Open Rinex observations file in read-only mode
            rin.open(obsFile, std::ios::in);

            rin.exceptions(ios::failbit);
            Rinex3ObsHeader roh;

            //read the header
            rin >> roh;
            gMap.header = roh;

            //read all epochs
            while (rin >> gRin)
            {

                if (gRin.body.size() == 0)
                {
                    printMsg(gRin.header.epoch, "Empty epoch record in Rinex file");
                    continue;
                }

                const auto& t = gRin.header.epoch;

                //keep only satellites from satellites systems selecyted for processing
                gRin.keepOnlySatSyst(opts().systems);

                //keep only types used for processing
                // gRin.keepOnlyTypeID(requireObs.getRequiredType());

                //compute approximate position
                if (firstTime)
                {
                    /* if (computeApprPos(gRin, data->SP3EphList, nominalPos))
                    continue;*/
                    Triple pos;
                    i = 0;
                    for (auto& it : confReader().getListValueAsDouble("nominalPosition", data->SiteRover))
                        pos[i++] = it;
                    nominalPos = Position(pos);

                 firstTime = false;
                }

                grDelayRover.setNominalPosition(nominalPos);

                tropoRovPtr.setAllParameters(t, nominalPos);
                ionoModel.setInitialRxPosition(nominalPos);

                corrRover.setNominalPosition(nominalPos);
                windupRover.setNominalPosition(nominalPos);
                svPcenterRover.setNominalPosition(nominalPos);
                model.rxPos = nominalPos;

                gRin >> requireObs;
                gRin >> CodeFilter;
                gRin >> SNRFilter;

                if (gRin.body.size() == 0)
                {
                    printMsg(gRin.header.epoch, "Rover receiver: all SV has been rejected.");

                    continue;
                }
                gRin >> computeLinear;
                gRin >> markCSLI2Rover;
                gRin >> markCSMW2Rover;
                //gRin >> snrCatcherL1Rover;
                gRin >> markArcRover;

                auto eop = data->eopStore.getEOP(MJD(t).mjd, IERSConvention::IERS2010);
                pole.setXY(eop.xp, eop.yp);

                gRin >> model;
                gRin >> eclipsedSV;
                gRin >> grDelayRover;
                gRin >> svPcenterRover;

                Triple tides(solid.getSolidTide(t, nominalPos) + ocean.getOceanLoading(data->SiteRover, t) + pole.getPoleTide(t, nominalPos));
                corrRover.setExtraBiases(tides);
                gRin >> corrRover;

                gRin >> windupRover;
                gRin >> computeTropoRover;

                gRin >> linearIonoFree;
                gRin >> oMinusC;

                DBOUT_LINE(">>" << CivilTime(gRin.header.epoch).asString());

                if (forwardBackwardCycles > 0)
                {
                    gRin >> solverFb;
                }
                else
                {
                    gRin >> solver;
                    GnssEpoch ep(gRin);
                    // updateNomPos(solverFB);
                    printSolution(ostream, solver, t, ep);
                    gMap.data.insert(std::make_pair(t, ep));
                }
            }
        }
        if (forwardBackwardCycles > 0)
        {
            cout << "Fw-Bw part started" << endl;
            solverFb.reProcess();
            gnssRinex gRin;
            cout << "Last process part started" << endl;
            while (solverFb.lastProcess(gRin))
            {
                GnssEpoch ep(gRin);
                //updateNomPos(solverFB);
                printSolution(ostream, solverFb, gRin.header.epoch, ep);
                gMap.data.insert(std::make_pair(gRin.header.epoch, ep));
            }
            cout << "Measurments rejected: " << solverFb.rejectedMeasurements << endl;
        }
    }



    void PppFloatSolution::updateRequaredObs()
    {
        LinearCombinations comm;
        bool useC1 = confReader().getValueAsBoolean("useC1");

        codeL1 = useC1 ? TypeID::C1 : TypeID::P1;
        computeLinear.setUseC1(useC1);
        computeLinear.add(make_unique<PDelta>());
        computeLinear.add(make_unique<MWoubenna>());

        computeLinear.add(make_unique<LDelta>());
        computeLinear.add(make_unique<LICombimnation>());

        configureSolver();

        requireObs.addRequiredType(codeL1);
        requireObs.addRequiredType(TypeID::P2);
        requireObs.addRequiredType(TypeID::L1);
        requireObs.addRequiredType(TypeID::L2);
        requireObs.addRequiredType(TypeID::LLI1);
        requireObs.addRequiredType(TypeID::LLI2);

        requireObs.addRequiredType(TypeID::S1);

        oMinusC.add(make_unique<PrefitPC>());
        oMinusC.add(make_unique<PrefitLC>());

        Equations->measTypes().insert(TypeID::prefitPC);
        Equations->measTypes().insert(TypeID::prefitLC);
        Equations->residTypes().insert(TypeID::postfitPC);
        Equations->residTypes().insert(TypeID::postfitLC);
    }

    void PppFloatSolution::configureSolver()
    {
        Equations->clearEquations();


        double qPrime = confReader().getValueAsDouble("tropoQ");
        Equations->addEquation(make_unique<TropoEquations>(qPrime));

        // White noise stochastic models
        auto  coord = make_unique<PositionEquations>();

        double posSigma = confReader().getValueAsDouble("posSigma");
        if (opts().dynamics == GnssDataStore::Dynamics::Static)
        {
            coord->setStochasicModel(make_shared<StochasticModel>());
        }
        else  if (opts().dynamics == GnssDataStore::Dynamics::Kinematic)
        {
            coord->setStochasicModel(make_shared<WhiteNoiseModel>(posSigma));
        }
        else if (opts().dynamics == GnssDataStore::Dynamics::RandomWalk)
        {
            for (const auto& it : coord->getParameters())
                coord->setStochasicModel(it, make_shared<RandomWalkModel>(posSigma));
        }

        //add position equations
        Equations->addEquation(std::move(coord));

        Equations->addEquation(std::make_unique<ClockBiasEquations>());

        if (opts().systems.size() > 1)
            Equations->addEquation(std::make_unique<InterSystemBias>());

        Equations->addEquation(std::make_unique<AmbiguitiesEquations>(TypeID::BLC));

        forwardBackwardCycles = confReader().getValueAsInt("forwardBackwardCycles");
    }


}
