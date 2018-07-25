#pragma once
#include "KalmanSolver.h"
#include"ProcessingClass.hpp"

namespace pod
{
    class KalmanSolverFB :
        public KalmanSolver
    {
    private:
        //set of all possible TypeID for code pseudorange postfit residuals 
        static const std::set<gpstk::TypeID> codeResTypes;

        //set of all possible TypeID for  carrier phase postfit residuals 
        static const std::set<gpstk::TypeID> phaseResTypes;


    public:
        KalmanSolverFB();
        KalmanSolverFB(eqComposer_sptr eqs);

        virtual ~KalmanSolverFB();

        virtual std::string getClassName() const override
        {
            return "KalmanSolverFB";
        }

        /// Solution
        virtual const  Vector<double>& Solution() const
        {
            return solver.Solution();
        }

        virtual  Vector<double>& Solution()
        {
            return solver.Solution();
        }

        /// Postfit-residuals.
        virtual const Vector<double>& PostfitResiduals() const override
        {
            return solver.PostfitResiduals();
        }
        virtual Vector<double>& PostfitResiduals() override
        {
            return solver.PostfitResiduals();
        }

        /// Covariance matrix
        virtual const Matrix<double>& CovMatrix()const
        {
            return solver.CovMatrix();
        }

        virtual Matrix<double>& CovMatrix()
        {
            return solver.CovMatrix();
        }

        //return sqrt(vpv/(n-p)) value
        virtual double getSigma() const
        {
            return solver.getSigma();
        }

        //return minimum number of satellites requared for state esimation
        virtual double getMinSatNumber() const
        {
            return solver.getMinSatNumber();
        }
        
        //return current solver  status 
        // true - solution valid
        // false - invalid
        virtual bool isValid() const
        {
            return solver.isValid();
        }

        //set minimum number of satellites requared for state esimation
        virtual KalmanSolverFB& setMinSatNumber(int value) override
        {
            solver.setMinSatNumber(value);
            return *this;
        }

        double getSolution(const FilterParameter& type) const override
        {
            return solver.getSolution(type);
        }

        double getVariance(const FilterParameter& type) const override
        {
            return solver.getVariance(type);
        }

        KalmanSolverFB& setLimits(const std::list<double>& codeLims, const std::list<double>& phaseLims);

        KalmanSolverFB& setCyclesNumber(size_t number)
        {
            cyclesNumber = number;
            return *this;
        }

        gpstk::gnssRinex & Process(gpstk::gnssRinex & gRin);


        bool lastProcess(gpstk::gnssRinex & gRin);

        //Reprocess the data stored during a previous 'Process()' call.
        void reProcess(int numCycles);

        //Reprocess the data stored during a previous 'Process()' call.
        void reProcess(void);

        //This method checks the residuals and modifies 'gData' accordingly.
    private:
        void checkLimits(gpstk::gnssRinex& gData, int cycleNumber);
        double getLimit(const gpstk::TypeID& type, int cycleNumber);

#pragma region Fields

        struct limits
        {
            std::vector<double> codeLimits;
            std::vector<double> phaseLimits;
        }tresholds;

        // Number of processed measurements.
    public: int processedMeasurements;

            //Number of measurements rejected because they were off limits.
    public: int rejectedMeasurements;

            //Number of measurements rejected because they were off limits.
    private: std::list<gpstk::gnssRinex> ObsData;

             // is current iteration first
    private: bool firstIteration;

    private: KalmanSolver solver;

    private: size_t cyclesNumber;

#pragma endregion


    };
}

