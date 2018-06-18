#include "EquationComposer.h"

using namespace gpstk;

namespace pod
{
   const std::map<gpstk::TypeID, double> EquationComposer::weigthFactors{

        //code pseudorange weight factor
        { TypeID::prefitC, 1.0  }      ,
        { TypeID::prefitP1, 1.0 }      ,
        { TypeID::prefitP2, 1.0 }      ,

        //carrier phase weight factor
        { TypeID::prefitL,  10000.0 },
        { TypeID::prefitL1, 10000.0 },
        { TypeID::prefitL2, 10000.0 },
    };

    void EquationComposer::Prepare(gnssRinex& gData)
    {
        //clear ambiguities set
        currAmb.clear();
        for (auto& eq : equations)
        {
            //prepare equations objects state
            eq->Prepare(gData);

            // update current set of ambiguities
            auto  ambs = eq->getAmbSet();
            currAmb.insert(ambs.cbegin(), ambs.cend());
        }
    }

    void EquationComposer::updateH(gpstk::gnssRinex& gData, gpstk::Matrix<double>& H)
    {
        int numSVs = gData.body.size();
        int numMeasTypes = measTypes().size();

        //number of measurements are equals number of satellites times observation types number 
        numMeas = numSVs*numMeasTypes;

        unknowns.clear();
        for (auto&& eq : equations)
        {
            auto&& t = eq->getParameters();
            unknowns.insert(t.cbegin(), t.cend());
        }
          

        numUnknowns = getNumUnknowns();

        // set resize design matrix 
        H.resize(numMeas, numUnknowns, 0.0);

        //get the 'core' variables part of design matrix 
        /*
        'hCore' matrix contains:
        |  T  | dX dY dZ | cdt | cdt(R1) | cdt(G2) | cdt(R2) |
        ------------------------------------------------------
        |     |          |     |         |         |         |
        |  m  | ax,ay,az |  1  |  R?1:0  |  G?1:0  |  R?1:0  |
        |     |          |     |         |         |         |
        ------------------------------------------------------
        */

        /*
        form the design martix H:
           | Tropo | dX dY dZ | cdt | cdt(R1) | cdt(G2) | cdt(R2) |     N1     |     N2     |
           ---------------------------------------------------------------------------------
           |       |          |     |         |         |         |            |            |
        P1 |   m   | ax,ay,az |  1  |  R?1:0  |    0    |    0    |     0      |      0     |
           |       |          |     |         |         |         |            |            |
           ----------------------------------------------------------------------------------
           |       |          |     |         |         |         |            |            |
        P2 |   m   | ax,ay,az |  1  |    0    |  G?1:0  |  R?1:0  |     0      |      0     |
           |       |          |     |         |         |         |            |            |
           ----------------------------------------------------------------------------------
           |       |          |     |         |         |         |            |            |
        L1 |   m   | ax,ay,az |  1  |  R?1:0  |    0    |    0    | lambda_1*E |      0     |
           |       |          |     |         |         |         |            |            |
           ----------------------------------------------------------------------------------
           |       |          |     |         |         |         |            |            |
        L2 |   m   | ax,ay,az |  1  |    0    |  G?1:0  |  R?1:0  |     0      | lambda_2*E |
           |       |          |     |         |         |         |            |            |
           ----------------------------------------------------------------------------------
        */
     
        int col(0);
        for (auto& eq : equations)
            eq->updateH(gData, measTypes(), H, col);
    }

    void EquationComposer::updatePhi(Matrix<double>& Phi) const
    {
        int i = 0;
        Phi.resize(numUnknowns, numUnknowns, 0.0);
        for (auto& eq : equations)
            eq->updatePhi(Phi, i);
    }

    void EquationComposer::updateQ(Matrix<double>& Q) const
    {
        int i = 0;
        Q.resize(numUnknowns, numUnknowns, 0.0);
        for (auto& eq : equations)
            eq->updateQ(Q, i);
    }

    void EquationComposer::updateW(const gnssRinex& gData, gpstk::Matrix<double>& weigthMatrix)
    {
        size_t  numsv = gData.body.size();
        // Generate the appropriate weights matrix
        // Try to extract weights from GDS
        satTypeValueMap dummy(gData.body.extractTypeID(TypeID::weight));

        //prepare identy matrix
        weigthMatrix.resize(numMeas, numMeas, 0.0);
        for (size_t i = 0; i < numMeas; i++)
            weigthMatrix(i, i) = 1.0;

        // Check if weights match
        if (dummy.numSats() == numsv)
        {
            auto weigths = gData.getVectorOfTypeID(TypeID::weight);

            for (size_t i = 0; i < numsv; i++)
                weigthMatrix(i, i) = weigthMatrix(i, i) * weigths(i);
        }
        else
        {
            size_t n(0);
            for (const auto& observable : measTypes())
            {
                const auto weigthFactor = weigthFactors.find(observable);
                if (weigthFactor == weigthFactors.end())
                {
                    std::string msg = "Can't find weigth factor for TypeID:: " 
                        + TypeID::tStrings[observable.type];

                    InvalidRequest e(msg);
                    GPSTK_THROW(e)
                }

                for (size_t i = 0; i < numsv; i++)
                    weigthMatrix(i+ numsv*n, i+ numsv*n) *= weigthFactor->second;
                n++;
            }
        }
    }

    void EquationComposer::updateMeas(const gnssRinex& gData, gpstk::Vector<double>& measVector)
    {
        measVector.resize(numMeas, 0.0);
        int j = 0;
        for (const auto& it : measTypes())
        {
            auto meas = gData.getVectorOfTypeID(it);
            int numSat = meas.size();
            for (int i = 0; i <numSat; i++)
                measVector(i + j*numSat) = meas(i);
            j++;
        }
    }

    int EquationComposer::getNumUnknowns() const
    {
        int res = 0;
        for (auto& eq : equations)
            res += eq->getNumUnknowns();
        return res;
    }

    void EquationComposer::updateKfState(gpstk::Vector<double>& currState, gpstk::Matrix<double>& currErrorCov) const
    {
        initKfState(currState, currErrorCov);

        int row = 0;

        //update state and covarince
        for (const auto& it_row : unknowns)
        {
            const auto& typeRow = filterData.find(it_row);
            if (typeRow != filterData.end())
            {
                currState(row) = filterData.at(it_row).value;
                int col = 0;
                for (const auto& it_col : unknowns)
                {
                    const auto& typeCol = (typeRow->second).valCov.find(it_col);
                    if (typeCol != (typeRow->second).valCov.end())
                        currErrorCov(col, row) = currErrorCov(row, col) = typeCol->second;
                    ++col;
                }
            }
            ++row;
        }
    }

    void EquationComposer::storeKfState(const gpstk::Vector<double>& currState, const gpstk::Matrix<double>& currErrorCov)
    {
        int row = 0;
        for (const auto& it_row : unknowns)
        {
            filterData[it_row].value = currState(row);

            int col = 0;
            for (const auto& it_col : unknowns)
            {
                filterData[it_row].valCov[it_col] = currErrorCov(row, col);
                ++col;
            }
            ++row;
        }
    }

    void EquationComposer::initKfState(gpstk::Vector<double>& state, gpstk::Matrix<double>& cov) const
    {
        state.resize(numUnknowns, 0.0);
        cov.resize(numUnknowns, numUnknowns, 0.0);

        int i = 0;
        for (auto& eq : equations)
            eq->defStateAndCovariance(state, cov, i);
    }

    void EquationComposer::saveResiduals(gpstk::gnssRinex& gData, gpstk::Vector<double>& postfitResiduals) const
    {
        int resNum = postfitResiduals.size();
        int satNum = gData.body.size();
        int numResTypes = (residTypes()).size();

        assert(satNum * numResTypes == resNum);

        int i_res = 0;
        for (const auto& resType : residTypes())
            for (auto& itSat : gData.body)
            {
                itSat.second[resType] = postfitResiduals(i_res);
                i_res++;
            }
    }
}