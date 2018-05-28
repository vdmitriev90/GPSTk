#include "KalmanSolver.h"
#include"WinUtils.h"
#include"PowerSum.hpp"
#include"ARSimple.hpp"
#include"ARMLambda.hpp"
#include <algorithm>

using namespace std;
using namespace gpstk;

namespace pod
{

    KalmanSolver::KalmanSolver():firstTime(true)
    { }
    KalmanSolver::KalmanSolver(eqComposer_sptr eqs) 
        :firstTime(true),equations(eqs)
    {}

    KalmanSolver::~KalmanSolver()
    {}

    gnssRinex& KalmanSolver::Process(gnssRinex& gData)
    {
        equations->Prepare(gData);

        while (true)
        {
            equations->updateH(gData, hMatrix);
            equations->updateMeas(gData, measVector);
            equations->updateW(gData, weigthMatrix);

            equations->updatePhi(phiMatrix);
            equations->updateQ(qMatrix);

            if (firstTime)
                equations->initKfState(solution, covMatrix);
            else
                equations->updateKfState(solution, covMatrix);

            firstTime = false;


            //kFilter.Reset(solution, covMatrix);
            DBOUT_LINE("----------------------------------------------------------------------------------------");
            DBOUT_LINE(CivilTime(gData.header.epoch));
            //auto svset = gData.getSatID();
            //for (auto& it:svset)
            //    DBOUT(it<<" ");

            //DBOUT_LINE("measVector\n" << setprecision(10) << measVector);
            //DBOUT_LINE("H\n" << hMatrix);
           
            //prepare
            Matrix<double> hMatrixTr = transpose(hMatrix);
            Matrix<double> phiMatrixTr = transpose(phiMatrix);
            Matrix<double> hTrTimesW = hMatrixTr*weigthMatrix;

            //predict
            Matrix<double> Pminus = phiMatrix*covMatrix*phiMatrixTr + qMatrix;
            Vector<double> xminus = phiMatrix*solution;
            
            //correct
            Matrix<double> invPminus = inverseChol(Pminus);
            covMatrix = inverseChol(hTrTimesW*hMatrix + invPminus);
            solution = covMatrix*(hTrTimesW*measVector + (invPminus*xminus));

            postfitResiduals = measVector - hMatrix * solution;
            DBOUT_LINE("Solution\n" << solution);
            DBOUT_LINE("postfitResiduals\n" << postfitResiduals);
            //DBOUT_LINE("CovPost\n" << covMatrix);
 
            equations->saveResiduals(gData, postfitResiduals);
            //
            fixAmbiguities(gData);
            storeAmbiguities(gData);

            break;

            if (!check(gData))
                break;
            reject(gData, eqComposer().residTypes());

            if (gData.body.size() < equations->getNumUnknowns())
            {
                equations->initKfState(solution, covMatrix);
                break;
            }
        }

        equations->storeKfState(solution, covMatrix);
        return gData;
    }
    SatID KalmanSolver::chooseRefSv(gnssRinex& gData) const
    {
        using type = decltype(gnssRinex::body)::value_type;
        auto svWithMaxEl = std::max_element(
            gData.body.begin(), gData.body.end(),
            [&](const type& it1, const type& it2)-> bool
        {
            double val1 = ::abs(it1.second.at(TypeID::elevation));
            double val2 = ::abs(it2.second.at(TypeID::elevation));
            return(val1 < val2);
        }
        );
        return svWithMaxEl->first;
    }
    
    void  KalmanSolver::fixAmbiguities(gnssRinex& gData)
    {
        int amb_num = equations->currentAmb().size();
        if (amb_num < 5) return;

        int core_num = equations->currentUnknowns().size();
        
        // find the reference SV
        SatID refSv = chooseRefSv(gData);
        const auto refSv_it = equations->currentAmb().find(refSv);
        // and it index
        int resv_index = std::distance(equations->currentAmb().begin(), refSv_it);
       
        Vector<double> coreParamsFloat(core_num, .0);
        for (int k = 0; k < core_num; k++)
            coreParamsFloat(k) = solution(k);

       
        Vector<double> DDfloatAmb(amb_num - 1, .0);
        Matrix<double> DDambCov(amb_num - 1, amb_num - 1, .0);
        Matrix<double> parDDAmbCov(core_num, amb_num - 1, .0);

        Matrix<double> SD2DD (covMatrix.rows() - 1, covMatrix.rows(), .0);
        for (int j = 0; j < core_num; j++)
            SD2DD(j, j) = 1.0;

        for (int j = core_num; j< SD2DD.rows(); j++)
        {
            SD2DD(j, resv_index + core_num) = -1.0;
            if (j<resv_index + core_num) SD2DD(j, j) = 1.0;
            else    SD2DD(j, j + 1) = 1.0;
        }
        DBOUT_LINE("SD2DD\n" << SD2DD);

        DBOUT_LINE("covSD\n"<<covMatrix)


        auto DDfloatSolution = SD2DD*solution;
        auto trSD2DD = transpose(SD2DD);
        auto DDCov = SD2DD*covMatrix*trSD2DD;
        DBOUT_LINE("covDD\n" << DDCov)

        ARMLambda ar;   

        for (int i = 0; i < amb_num-1; i++)
        {
            DDfloatAmb(i) = DDfloatSolution(core_num + i);
            for (int j = 0; j < amb_num-1; j++)
                DDambCov(i, j) = DDCov(core_num + i, core_num + j);

            for (int k = 0; k < core_num; k++)
                parDDAmbCov(k, i) = DDCov(k, core_num + i);
        }

        auto DDfixedAmb = ar.resolveIntegerAmbiguity(DDfloatAmb, DDambCov);
        DBOUT_LINE("float DD amb\n" << DDfloatAmb)
        DBOUT_LINE("fixed DD amb\n"<< DDfixedAmb)
        auto coreParamsFixed = coreParamsFloat - parDDAmbCov*inverseChol(DDambCov)*(DDfloatAmb - DDfixedAmb);

        for (int k = 0; k < core_num; k++)
             solution(k) = coreParamsFixed(k);
    }

    void  KalmanSolver::storeAmbiguities(gnssRinex& gData) const
    {
        int i(0);
        for (const auto & sv : equations->currentAmb())
        {
            auto & it = gData.body.find(sv);
            if (it != gData.body.end())
                gData.body[sv][TypeID::BL1] = solution(equations->currentUnknowns().size() + i);
            ++i;
        }
    }

    int KalmanSolver::check(gnssRinex& gData)
    {
        Matrix<double> res(PostfitResiduals().size(), 1, 0.0);

        res = res.assignFrom(PostfitResiduals());
        
        //compute v'pv
        auto vpv = transpose(res)*weigthMatrix*res;

        double sigma = sqrt(vpv(0,0));

        double varX = getVariance(TypeID::dx);     // Cov dx    - #8
        double varY = getVariance(TypeID::dy);     // Cov dy    - #9
        double varZ = getVariance(TypeID::dz);     // Cov dz    - #10
        double stDev3D = sqrt(varX + varY + varZ);

        if (sigma / stDev3D > 3)
        {
            cout << "Epoch: " << CivilTime(gData.header.epoch) << " catched " << endl;
            cout << "sigma: " << sigma << " sigma: " << stDev3D << endl;
            return 1;
        }
        else
            return 0;
    }
    
    //reject by code postfit residual 
    gnssRinex& KalmanSolver::reject(gnssRinex& gData, const TypeIDList&  typeIds)
    {
        using type = decltype(gnssRinex::body)::value_type;
        SatIDSet rejSat;
        ///!!!
        ///here we use only last type of postfit residuals, because in case of
        ///combined code/phase measurements processing this part contans carrier phase residuals
        //in case code/phase only processing we have only one element in @typeIds, which one will be used
        ///!!!
        const auto& id = typeIds.rbegin();

        //get the sv - typeMap pair with largest residual value

        auto svWithMaxResidual = std::max_element(
            gData.body.begin(), gData.body.end(),
            [&](const type& it1, const type& it2)-> bool
        {
            double val1 = ::abs(it1.second.at(*id));
            double val2 = ::abs(it2.second.at(*id));
            return(val1 < val2);
        }
        );

        //report detection
        cout << "Removed SV: " << svWithMaxResidual->first;
        cout << " with " << TypeID::tStrings[id->type] << " = ";
        cout << svWithMaxResidual->second[*id] << endl;
        //remove sv
        rejSat.insert(svWithMaxResidual->first);


        gData.removeSatID(rejSat);

        return gData;
    }

    gnssSatTypeValue& KalmanSolver::Process(gnssSatTypeValue& gData)
        throw(ProcessingException)
    {
        try
        {
            // Build a gnssRinex object and fill it with data
            gnssRinex g1;
            g1.header = gData.header;
            g1.body = gData.body;

            // Call the Process() method with the appropriate input object
            Process(g1);

            // Update the original gnssSatTypeValue object with the results
            gData.body = g1.body;

            return gData;

        }
        catch (Exception& u)
        {
            // Throw an exception if something unexpected happens
            ProcessingException e(getClassName() + ":" + u.what());

            GPSTK_THROW(e);
        }
    }

    double KalmanSolver::getSolution(const TypeID& type) const
    {
        // Define counter
        int counter(0);

        for (const auto it2 : equations->currentUnknowns())
        {
            if (it2 == type)
                return solution(counter);
            counter++;
        }

        InvalidRequest e("Type: '" + TypeID::tStrings.at(type.type) + "' not found in  current set of unknowns.");
        GPSTK_THROW(e);

    }  // End of method 'SolverLMS::getSolution()'

    double KalmanSolver::getVariance(const TypeID& type) const
    {
        int counter(0);

        for (const auto it2 : equations->currentUnknowns())
        {
            if (it2 == type)
                return covMatrix(counter, counter);
            ++counter;
        }

        InvalidRequest e("Type: '" + TypeID::tStrings.at(type.type)+"' not found in current set of unknowns.");
        GPSTK_THROW(e);

    }  // End of method 'SolverLMS::getVariance()'
}