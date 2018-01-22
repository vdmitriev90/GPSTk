#include"PPPSolutionBase.h"

#include<list>
#include<set>
#include <direct.h>
#include<windows.h>
#include<regex>

#include"GnssEpochMap.h"
#include"PPPSolution.h"
#include"PODSolution.h"
#include"auxiliary.h"


#include "Rinex3NavHeader.hpp"
#include "Rinex3NavData.hpp"
#include "Rinex3NavStream.hpp"

namespace pod
{
    PPPSolutionBase* PPPSolutionBase::Factory(bool isSpaceborne, ConfDataReader & reader, const string &  dir)
    {
        if (isSpaceborne)
            return new PODSolution(reader, dir);
        else
            return new PPPSolution(reader, dir);
    }

    PPPSolutionBase::PPPSolutionBase(ConfDataReader & cReader,string dir ) : confReader(&cReader), workingDir(dir)
    {
    } 
    PPPSolutionBase:: ~PPPSolutionBase()
    {
        solverPR.release();
    }

    void PPPSolutionBase::mapSNR(gnssRinex & gRin)
    {
        for (auto &it1 : gRin.body)
        {
            auto  ts1 = TypeID(TypeID::S1);
            auto  ts2 = TypeID(TypeID::S2);
           
            auto s1 = it1.second.find(ts1);
            if (s1 != it1.second.end())
                s1->second =  mapSNR(s1->second);

            auto s2 = it1.second.find(ts1);
            if (s2 != it1.second.end())
                s2->second = mapSNR(s2->second);
        }
    }

    void  PPPSolutionBase::LoadData()
    {
        try
        {
            maskEl = confReader->getValueAsDouble("ElMask");
           
            solverPR->maskSNR = maskSNR = confReader->getValueAsDouble("SNRmask");

            cout << "mask El " << maskEl << endl;
            cout << "mask SNR " << (int)maskSNR << endl;

            systems.insert(SatID::SatelliteSystem::systemGPS);
            bool useGLN = confReader->getValueAsBoolean("useGLN");
            if (useGLN)
                systems.insert(SatID::SatelliteSystem::systemGlonass);

            cout << "Used Sat. Systems: ";
            for (auto& ss : systems)
                cout << SatID::convertSatelliteSystemToString(ss) << " ";
            cout << endl;

            //set BCE files direcory 
            bceDir = confReader->getValue("RinexNavFilesDir");
            //set generic files direcory 
            string subdir = confReader->getValue("GenericFilesDir");
            genFilesDir = workingDir + "\\" + subdir + "\\";

            subdir = confReader->getValue("RinesObsDir");
            auxiliary::getAllFilesInDir(workingDir + "\\" + subdir, rinexObsFiles);

            cout << "Ephemeris Loading... ";
            cout << loadEphemeris() << endl;

            calcApprPos = confReader->getValueAsBoolean("calcApprPos");
            apprPosFile = confReader->getValue("apprPosFile");

            this->dynamics = (Dynamics)confReader->getValueAsInt("Dynamics");

            //load clock data from RINEX clk files, if required
            if (confReader->getValueAsBoolean("UseRinexClock"))
            {
                cout << "Rinex clock data Loading... ";
                cout << loadClocks() << endl;
            }

            cout << "IonoModel Loading... ";
            cout << loadIono() << endl;

            cout << "Load Glonass FCN data... ";
            cout << loadFcn() << endl;

            cout << "Load Earth orientation data... ";
            cout << loadEOPData() << endl;

        }
        catch (const Exception& e)
        {
            cout << "Failed to load input data. An error has occured: " <<e.what() << endl;
            exit(-1);
        }
        catch (const std::exception& e)
        {
            cout << "failed to load input data: An error has occured: " << e.what() << endl;
            exit(-1);
        }
    }
    
    //
    bool PPPSolutionBase::loadEphemeris()
    {
        // Set flags to reject satellites with bad or absent positional
        // values or clocks
        SP3EphList.clear();
        SP3EphList.rejectBadPositions(true);
        SP3EphList.rejectBadClocks(true);

        list<string> files;
        string subdir = confReader->getValue("EphemerisDir");
        auxiliary::getAllFilesInDir(workingDir+"\\"+ subdir, files);

        for (auto file : files)
        {
            // Try to load each ephemeris file
            try
            {
                SP3EphList.loadFile(file);
            }
            catch (FileMissingException& e)
            {
                // If file doesn't exist, issue a warning
                cerr << "SP3 file '" << file << "' doesn't exist or you don't "
                    << "have permission to read it. Skipping it." << endl;
                exit(-1);
            }
        }
        return files.size() > 0;
    }
     //reading clock data
    bool PPPSolutionBase::loadClocks()
    {
        list<string> files;
        string subdir = confReader->getValue("RinexClockDir");
        auxiliary::getAllFilesInDir(workingDir+"\\" + subdir, files);

        for (auto file : files)
        {
            // Try to load each ephemeris file
            try
            {
                SP3EphList.loadRinexClockFile(file);
            }
            catch (FileMissingException& e)
            {
                // If file doesn't exist, issue a warning
                cerr << "Rinex clock file '" << file << "' doesn't exist or you don't "
                    << "have permission to read it. Skipping it." << endl;
                exit(-1);
            }
        }//   while ((ClkFile = confReader->fetchListValue("rinexClockFiles", station)) != "")
        return files.size()>0;
    }

    bool  PPPSolutionBase::loadIono()
    {
        const string gpsObsExt = ".[\\d]{2}[nN]";
        list<string> files;

        auxiliary::getAllFilesInDir(workingDir+"\\"+ bceDir, gpsObsExt, files);
        int i = 0;
        for (auto file : files)
        {
            try
            {
                IonoModel iMod;
                Rinex3NavStream rNavFile;
                Rinex3NavHeader rNavHeader;

                rNavFile.open(file.c_str(), std::ios::in);
                rNavFile >> rNavHeader;

                #pragma region try get the date

                CommonTime refTime = CommonTime::BEGINNING_OF_TIME;
                if (rNavHeader.fileAgency == "AIUB")
                {

                    for (auto it : rNavHeader.commentList)
                    {
                        int doy = -1, yr = -1;
                        std::tr1::cmatch res;
                        std::tr1::regex rxDoY("DAY [0-9]{3}"), rxY(" [0-9]{4}");
                        bool b = std::tr1::regex_search(it.c_str(), res, rxDoY);
                        if (b)
                        {
                            string sDay = res[0];
                            sDay = sDay.substr(sDay.size() - 4, 4);
                            doy = stoi(sDay);
                        }
                        if (std::tr1::regex_search(it.c_str(), res, rxY))
                        {
                            string sDay = res[0];
                            sDay = sDay.substr(sDay.size() - 5, 5);
                            yr = stoi(sDay);
                        }
                        if (doy > 0 && yr > 0)
                        {
                            refTime = YDSTime(yr, doy, 0, TimeSystem::GPS);
                            break;
                        }
                    }
                }
                else
                {
                    long week = rNavHeader.mapTimeCorr["GPUT"].refWeek;
                    if (week > 0)
                    {
                        GPSWeekSecond gpsws = GPSWeekSecond(week, 0);
                        refTime = gpsws.convertToCommonTime();
                    }
                }
                #pragma endregion

                if (rNavHeader.valid & Rinex3NavHeader::validIonoCorrGPS)
                {
                    // Extract the Alpha and Beta parameters from the header
                    double* ionAlpha = rNavHeader.mapIonoCorr["GPSA"].param;
                    double* ionBeta = rNavHeader.mapIonoCorr["GPSB"].param;

                    // Feed the ionospheric model with the parameters
                    iMod.setModel(ionAlpha, ionBeta);
                    i++;
                }
                else
                {
                    cerr << "WARNING: Navigation file " << file
                        << " doesn't have valid ionospheric correction parameters." << endl;
                    exit(-1);
                }

                ionoStore.addIonoModel(refTime, iMod);
            }
            catch (...)
            {
                cerr << "Problem opening file " << file << endl;
                cerr << "Maybe it doesn't exist or you don't have proper read "
                    << "permissions." << endl;
                exit(-1);
            }
        }
        return  i > 0;
    }

    bool  PPPSolutionBase::loadFcn()
    {
        const string glnObsExt = ".[\\d]{2}[gG]";
        list<string> files;
        auxiliary::getAllFilesInDir(workingDir + "\\" + bceDir, glnObsExt, files);
        int i = 0;
        for (auto file : files)
        {
            try
            {
                Rinex3NavStream rNavFile;
                Rinex3NavHeader rNavHeader;

                rNavFile.open(file.c_str(), std::ios::in);
                rNavFile >> rNavHeader;
                Rinex3NavData nm;
                while (rNavFile >> nm)
                {
                    SatID::glonassFcn[SatID(nm.sat)] = nm.freqNum;
                    i++;
                }
            }
            catch (...)
            {
                cerr << "Problem opening file " << file << endl;
                cerr << "Maybe it doesn't exist or you don't have proper read "
                    << "permissions." << endl;
                exit(-1);
            }
        }
        return i > 0;
    }

    bool PPPSolutionBase::loadEOPData()
    {

        string iersEopFile = genFilesDir;
        try 
        {
            iersEopFile += confReader->getValue("IersEopFile");
        }
        catch (...)
        {
            cerr << "Problem get value from config: file \"IersEopFile\" " << endl;
            exit(-1);
        }

        try
        {         
            eopStore.addIERSFile(iersEopFile);
        }
        catch (...)
        {
            cerr << "Problem opening file " << iersEopFile << endl;
            cerr << "Maybe it doesn't exist or you don't have proper read "
                << "permissions." << endl;
            exit(-1);
        }
        return eopStore.size() > 0;
    }

    bool PPPSolutionBase::loadCodeBiades()
    {
        string biasesFile = genFilesDir;
        try
        {
            biasesFile += confReader->getValue("IersEopFile");
        }
        catch (...)
        {
            cerr << "Problem get value from config: file \"IersEopFile\" " << endl;
            exit(-1);
        }

        try
        {
           // DCBData.setDCBFile(biasesFile,);
        }
        catch (...)
        {
            cerr << "Problem opening file " << biasesFile << endl;
            cerr << "Maybe it doesn't exist or you don't have proper read "
                << "permissions." << endl;
            exit(-1);
        }
        return eopStore.size() > 0;
    }

    void PPPSolutionBase::checkObservable()
    {
        string subdir = confReader->getValue("RinesObsDir");
        auxiliary::getAllFilesInDir(workingDir + "\\" + subdir, rinexObsFiles);

        ofstream os(workingDir+ "\\ObsStatisic.out");

        for (auto obsFile : rinexObsFiles)
        {

            //Input observation file stream
            Rinex3ObsStream rin;
            // Open Rinex observations file in read-only mode
            rin.open(obsFile, std::ios::in);

            rin.exceptions(ios::failbit);
            Rinex3ObsHeader roh;
            Rinex3ObsData rod;

            //read the header
            rin >> roh;

            while (rin >> rod)
            {
                if (rod.epochFlag == 0 || rod.epochFlag == 1)  // Begin usable data
                {
                    int NumC1(0), NumP1(0), NumP2(0), NumBadCNo1(0);
                    os << setprecision(12) << (CivilTime)rod.time << " " ;
                    int nGPS = 0, nGLN = 0;
                    for (auto &it : rod.obs)
                    {
                        if (systems.find(it.first.system) == systems.end()) continue;
                        if (it.first.system == SatID::SatelliteSystem::systemGPS)
                            nGPS++;
                        if (it.first.system == SatID::SatelliteSystem::systemGlonass)
                            nGLN++;

                        auto &ids = CodeProcSvData::obsTypes[it.first.system];

                        double C1 = rod.getObs(it.first, ids[TypeID::C1], roh).data;

                        int CNoL1 = rod.getObs(it.first, ids[TypeID::S1], roh).data;
                        if (CNoL1 < 30) NumBadCNo1++;

                        double P1 = rod.getObs(it.first, ids[TypeID::P1], roh).data;
                        if (P1 > 0.0)  NumP1++;

                        double P2 = rod.getObs(it.first, ids[TypeID::P2], roh).data;
                        if (P2 > 0.0)  NumP2++;
                    }

                    os <<nGPS<<" "<<nGLN<<" "<< NumBadCNo1 << " " << NumP1 << " " << NumP2 << endl;
                }
            }
        }
    }

    void PPPSolutionBase::PRProcess()
    {
        NeillTropModel NeillModel = solverPR->initTropoModel(nominalPos, DoY);

        apprPos.clear();

        cout << "solverType " << solverPR->getName() << endl;

        solverPR->maskEl = 5;
        solverPR->ionoType = (CodeIonoCorrType)confReader->getValueAsInt("CodeIonoCorrType");

        ofstream os;
        string outPath = workingDir + "\\" + apprPosFile;
        os.open(outPath);
       
        //decimation
        int sampl(1);
        double tol(0.1);

        for (auto obsFile : rinexObsFiles)
        {
            int badSol(0);
            cout << obsFile << endl;
            try 
            {
                //Input observation file stream
                Rinex3ObsStream rin;
                // Open Rinex observations file in read-only mode
                rin.open(obsFile, std::ios::in);

                rin.exceptions(ios::failbit);
                Rinex3ObsHeader roh;
                Rinex3ObsData rod;

                //read the header
                rin >> roh;
                CommonTime ct0 = roh.firstObs;
                CommonTime Tpre( CommonTime::BEGINNING_OF_TIME);
                Tpre.setTimeSystem(TimeSystem::Any);
                // Let's process all lines of observation data, one by one
                while (rin >> rod)
                {
                    //work around for post header comments
                    if (std::abs(rod.time - Tpre) <= CommonTime::eps) continue;
                    Tpre = rod.time;
                    double dt = rod.time - ct0;
                    GPSWeekSecond gpst = static_cast<GPSWeekSecond>(rod.time);

                    if (fmod(gpst.getSOW(), sampl) > tol) continue;

                    int GoodSats = 0;
                    int res = 0;
                    CodeProcSvData svData;
                    ///
                    solverPR->selectObservables(rod, roh, systems, CodeProcSvData::obsTypes, svData);
                    
                    for (auto &it : svData.data)
                        it.second.snr = mapSNR(it.second.snr);
                    
                    svData.applyCNoMask(solverPR->maskSNR);
                    GoodSats = svData.getNumUsedSv();

                    os  << setprecision(6);
                    os  << CivilTime(gpst).printf("%02Y %02m %02d %02H %02M %02S %P") << " "<<dt<< " ";

                    if (GoodSats >= 4)
                    {
                        try
                        {
                            solverPR->prepare(rod.time, SP3EphList, svData);
                            res = solverPR->solve(rod.time, ionoStore, svData);
                            os << res;
                        }
                        catch (Exception &e)
                        {
                            GPSTK_RETHROW(e)
                        }
                    }
                    else
                        res = -1;

                    os << *solverPR<<" "<<svData << endl;

                    if (res == 0)
                    {
                        Xvt xvt;
                        xvt.x = Triple(solverPR->Sol(0), solverPR->Sol(1), solverPR->Sol(2));
                        xvt.clkbias = solverPR->Sol(3);
                        apprPos.insert(pair<CommonTime, Xvt>(rod.time, xvt));
                    }
                    else
                    {
                        solverPR->Sol = 0.0;
                        badSol++;
                    }
                }
                rin.close();
            }
            catch (Exception& e)
            {
                cerr << e << endl;
                GPSTK_RETHROW(e);
            }
            catch (...)
            {
                cerr << "Caught an unexpected exception." << endl;
            }
            cout << "Number of bad solutions for file " << badSol << endl;
        }
    }
    //
    void PPPSolutionBase::process()
    {

        if (calcApprPos)
        {
            PRProcess();
        }
        else
        {
            string appr_pos_file = workingDir + "\\" + apprPosFile;
            cout << "Approximate Positions loading from \n" + appr_pos_file + "\n... ";

            loadApprPos(appr_pos_file);
            cout << "\nComplete." << endl;
        }
        try
        {
            processCore();
        }
        catch (ConfigurationException &conf_exp)
        {
            cerr << conf_exp.what() << endl;
            throw;
        }
        catch (Exception &gpstk_e)
        {
            GPSTK_RETHROW(gpstk_e);
        }
        catch (std::exception &std_e)
        {
            cerr << std_e.what() << endl;
            throw;
        }
    }

    // Method to print solution values
    void PPPSolutionBase::printSolution(ofstream& outfile,
        const SolverPPP& solver,
        const CommonTime& time,
        const ComputeDOP& cDOP,
        GnssEpoch &   gEpoch,
        double dryTropo,
        int   precision,
        const Position &nomXYZ)
    {
        // Prepare for printing
        outfile << fixed << setprecision(precision);

        // Print results
        outfile << static_cast<YDSTime>(time).year << "-";   // Year           - #1
        outfile << static_cast<YDSTime>(time).doy << "-";    // DayOfYear      - #2
        outfile << static_cast<YDSTime>(time).sod << "  ";   // SecondsOfDay   - #3
        outfile << setprecision(6) << (static_cast<YDSTime>(time).doy + static_cast<YDSTime>(time).sod / 86400.0) << "  " << setprecision(precision);

        //calculate statistic
        double x(0), y(0), z(0), varX(0), varY(0), varZ(0);

        // We add 0.1 meters to 'wetMap' because 'NeillTropModel' sets a
        // nominal value of 0.1 m. Also to get the total we have to add the
        // dry tropospheric delay value
        // ztd - #7
        double wetMap = solver.getSolution(TypeID::wetMap) + 0.1 + dryTropo;

        gEpoch.slnData.insert(pair<TypeID, double>(TypeID::recZTropo, wetMap));


        x = nomXYZ.X() + solver.getSolution(TypeID::dx);    // dx    - #4
        y = nomXYZ.Y() + solver.getSolution(TypeID::dy);    // dy    - #5
        z = nomXYZ.Z() + solver.getSolution(TypeID::dz);    // dz    - #6

        gEpoch.slnData.insert(pair<TypeID, double>(TypeID::recX, x));
        gEpoch.slnData.insert(pair<TypeID, double>(TypeID::recY, y));
        gEpoch.slnData.insert(pair<TypeID, double>(TypeID::recZ, z));

        varX = solver.getVariance(TypeID::dx);     // Cov dx    - #8
        varY = solver.getVariance(TypeID::dy);     // Cov dy    - #9
        varZ = solver.getVariance(TypeID::dz);     // Cov dz    - #10

        double cdt = solver.getSolution(TypeID::cdt);
        gEpoch.slnData.insert(pair<TypeID, double>(TypeID::recCdt, cdt));


        //
        outfile << x << "  " << y << "  " << z << "  " << cdt << " ";

        auto defeq = solver.getDefaultEqDefinition();

        auto itcdtGLO = defeq.body.find(TypeID::recCdtGLO);
        if (defeq.body.find(TypeID::recCdtGLO) != defeq.body.end())
        {
            double cdtGLO = solver.getSolution(TypeID::recCdtGLO);
            gEpoch.slnData.insert(pair<TypeID, double>(TypeID::recCdtGLO, cdtGLO));

            outfile << cdtGLO << " ";
        }

        if (defeq.body.find(TypeID::recCdtdot) != defeq.body.end())
        {
            double recCdtdot = solver.getSolution(TypeID::recCdtdot);
            gEpoch.slnData.insert(pair<TypeID, double>(TypeID::recCdtdot, recCdtdot));

            outfile <<setprecision(12) << recCdtdot << " ";
        }
        double sigma = sqrt(varX + varY + varZ);
        gEpoch.slnData.insert(pair<TypeID, double>(TypeID::sigma, sigma));
        outfile << setprecision(6) <<wetMap << "  " << sigma << "  ";

        gEpoch.slnData.insert(pair<TypeID, double>(TypeID::recSlnType, 16));

        //gEpoch.slnData.insert(pair<TypeID, double>(TypeID::recPDOP, cDOP.getPDOP()));
        outfile << gEpoch.satData.size() << endl;    // Number of satellites - #12

        return;

    }  // End of method 'ex9::printSolution()'

    void PPPSolutionBase::updateNomPos(const CommonTime& time, Position &nominalPos)
    {
        if (dynamics == Dynamics::Kinematic)
        {
            auto it_pos = apprPos.find(time);
            if (it_pos != apprPos.end())
                nominalPos = it_pos->second;
        }
    }
    
    bool PPPSolutionBase::loadApprPos(std::string path)
    {
        apprPos.clear();
        try
        {
            ifstream file(path);
            if (file.is_open())
            {
                unsigned int Y(0), m(0), d(0), D(0), M(0), S(0);
                double sow(0.0), x(0.0), y(0.0), z(0.0), rco(0.0),dt(0);
                int solType(0);
                string sTS;

                string line;
                while (file >> Y >> m >> d >> D >> M >> S >> sTS >> dt>> solType >> x >> y >> z >> rco)
                {
                    if (!solType)
                    {
                        CivilTime ct = CivilTime(Y, m, d, D, M, S, TimeSystem::GPS);
                        CommonTime time = static_cast<CommonTime> (ct);
                        Xvt xvt;
                        xvt.x = Triple(x, y, z);
                        xvt.clkbias = rco;
                        apprPos.insert(pair<CommonTime, Xvt>(time, xvt));
                    }
                    string line;
                    getline(file, line);
                }
            }
            else
            {
                auto mess = "Can't load data from file: " + path;
                std::exception e(mess.c_str());
                throw e;
            }
        }
        catch (const std::exception& e)
        {
            cout << e.what() << endl;
            throw e;
        }
        return true;
    }

}