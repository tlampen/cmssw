//
//

#include "PhysicsTools/PatAlgos/plugins/PATPhotonProducer.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"
#include "DataFormats/Common/interface/View.h"
#include "DataFormats/HepMCCandidate/interface/GenParticleFwd.h"
#include "DataFormats/HepMCCandidate/interface/GenParticle.h"

#include "DataFormats/EcalDetId/interface/EcalSubdetector.h"
#include "RecoEcal/EgammaCoreTools/interface/EcalClusterLazyTools.h"
#include "Geometry/CaloEventSetup/interface/CaloTopologyRecord.h"
#include "Geometry/CaloTopology/interface/CaloTopology.h"
#include "Geometry/Records/interface/CaloGeometryRecord.h"

#include "FWCore/ParameterSet/interface/ConfigurationDescriptions.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescription.h"

#include "RecoEgamma/EgammaTools/interface/ConversionTools.h"
#include "RecoEcal/EgammaCoreTools/interface/EcalClusterTools.h"

#include "TVector2.h"
#include "DataFormats/Math/interface/deltaR.h"

#include <memory>

using namespace pat;

PATPhotonProducer::PATPhotonProducer(const edm::ParameterSet & iConfig) :
  isolator_(iConfig.exists("userIsolation") ? iConfig.getParameter<edm::ParameterSet>("userIsolation") : edm::ParameterSet(), consumesCollector(), false) ,
  useUserData_(iConfig.exists("userData"))
{
  // initialize the configurables
  photonToken_ = consumes<edm::View<reco::Photon> >(iConfig.getParameter<edm::InputTag>("photonSource"));
  electronToken_ = consumes<reco::GsfElectronCollection>(edm::InputTag("gedGsfElectrons"));
  hConversionsToken_ = consumes<reco::ConversionCollection>(edm::InputTag("allConversions"));
  beamLineToken_ = consumes<reco::BeamSpot>(iConfig.getParameter<edm::InputTag>("beamLineSrc"));
  embedSuperCluster_ = iConfig.getParameter<bool>("embedSuperCluster");
  embedSeedCluster_ = iConfig.getParameter<bool>( "embedSeedCluster" );
  embedBasicClusters_ = iConfig.getParameter<bool>( "embedBasicClusters" );
  embedPreshowerClusters_ = iConfig.getParameter<bool>( "embedPreshowerClusters" );
  embedRecHits_ = iConfig.getParameter<bool>( "embedRecHits" );
  reducedBarrelRecHitCollection_ = iConfig.getParameter<edm::InputTag>("reducedBarrelRecHitCollection");
  reducedBarrelRecHitCollectionToken_ = mayConsume<EcalRecHitCollection>(reducedBarrelRecHitCollection_);
  reducedEndcapRecHitCollection_ = iConfig.getParameter<edm::InputTag>("reducedEndcapRecHitCollection");
  reducedEndcapRecHitCollectionToken_ = mayConsume<EcalRecHitCollection>(reducedEndcapRecHitCollection_);
  // MC matching configurables
  addGenMatch_ = iConfig.getParameter<bool>( "addGenMatch" );
  if (addGenMatch_) {
    embedGenMatch_ = iConfig.getParameter<bool>( "embedGenMatch" );
    if (iConfig.existsAs<edm::InputTag>("genParticleMatch")) {
      genMatchTokens_.push_back(consumes<edm::Association<reco::GenParticleCollection> >(iConfig.getParameter<edm::InputTag>( "genParticleMatch" )));
    }
    else {
      genMatchTokens_ = edm::vector_transform(iConfig.getParameter<std::vector<edm::InputTag> >( "genParticleMatch" ), [this](edm::InputTag const & tag){return consumes<edm::Association<reco::GenParticleCollection> >(tag);});
    }
  }
  // Efficiency configurables
  addEfficiencies_ = iConfig.getParameter<bool>("addEfficiencies");
  if (addEfficiencies_) {
    efficiencyLoader_ = pat::helper::EfficiencyLoader(iConfig.getParameter<edm::ParameterSet>("efficiencies"), consumesCollector());
  }
  // photon ID configurables
  addPhotonID_ = iConfig.getParameter<bool>( "addPhotonID" );
  if (addPhotonID_) {
    // it might be a single photon ID
    if (iConfig.existsAs<edm::InputTag>("photonIDSource")) {
      photIDSrcs_.push_back(NameTag("", iConfig.getParameter<edm::InputTag>("photonIDSource")));
    }
    // or there might be many of them
    if (iConfig.existsAs<edm::ParameterSet>("photonIDSources")) {
      // please don't configure me twice
      if (!photIDSrcs_.empty()){
	throw cms::Exception("Configuration") << "PATPhotonProducer: you can't specify both 'photonIDSource' and 'photonIDSources'\n";
      }
      // read the different photon ID names
      edm::ParameterSet idps = iConfig.getParameter<edm::ParameterSet>("photonIDSources");
      std::vector<std::string> names = idps.getParameterNamesForType<edm::InputTag>();
      for (std::vector<std::string>::const_iterator it = names.begin(), ed = names.end(); it != ed; ++it) {
	photIDSrcs_.push_back(NameTag(*it, idps.getParameter<edm::InputTag>(*it)));
      }
    }
    // but in any case at least once
    if (photIDSrcs_.empty()) throw cms::Exception("Configuration") <<
      "PATPhotonProducer: id addPhotonID is true, you must specify either:\n" <<
      "\tInputTag photonIDSource = <someTag>\n" << "or\n" <<
      "\tPSet photonIDSources = { \n" <<
      "\t\tInputTag <someName> = <someTag>   // as many as you want \n " <<
      "\t}\n";
  }
  photIDTokens_ = edm::vector_transform(photIDSrcs_, [this](NameTag const & tag){return mayConsume<edm::ValueMap<Bool_t> >(tag.second);});
  // Resolution configurables
  addResolutions_ = iConfig.getParameter<bool>("addResolutions");
  if (addResolutions_) {
    resolutionLoader_ = pat::helper::KinResolutionsLoader(iConfig.getParameter<edm::ParameterSet>("resolutions"));
  }
  // Check to see if the user wants to add user data
  if ( useUserData_ ) {
    userDataHelper_ = PATUserDataHelper<Photon>(iConfig.getParameter<edm::ParameterSet>("userData"), consumesCollector());
  }
  // produces vector of photons
  produces<std::vector<Photon> >();

  // read isoDeposit labels, for direct embedding
  readIsolationLabels(iConfig, "isoDeposits", isoDepositLabels_, isoDepositTokens_);
  // read isolation value labels, for direct embedding
  readIsolationLabels(iConfig, "isolationValues", isolationValueLabels_, isolationValueTokens_);

}

PATPhotonProducer::~PATPhotonProducer() {
}

void PATPhotonProducer::produce(edm::Event & iEvent, const edm::EventSetup & iSetup)
{
  // switch off embedding (in unschedules mode)
  if (iEvent.isRealData()){
    addGenMatch_   = false;
    embedGenMatch_ = false;
  }

  edm::ESHandle<CaloTopology> theCaloTopology;
  iSetup.get<CaloTopologyRecord>().get(theCaloTopology);
  ecalTopology_ = & (*theCaloTopology);

  // Get the vector of Photon's from the event
  edm::Handle<edm::View<reco::Photon> > photons;
  iEvent.getByToken(photonToken_, photons);

  edm::InputTag  reducedEBRecHitCollection(std::string("reducedEcalRecHitsEB"));
  edm::InputTag  reducedEERecHitCollection(std::string("reducedEcalRecHitsEE"));
  EcalClusterLazyTools lazyTools(iEvent, iSetup, reducedBarrelRecHitCollection_, reducedEndcapRecHitCollection_);

  // Get calo geometry
  edm::ESHandle<CaloGeometry> theCaloGeometry;
  iSetup.get<CaloGeometryRecord>().get(theCaloGeometry);
  ecalGeometry_ = & (*theCaloGeometry);

  // for conversion veto selection
  edm::Handle<reco::ConversionCollection> hConversions;
  iEvent.getByToken(hConversionsToken_, hConversions);

  // Get the collection of electrons from the event
  edm::Handle<reco::GsfElectronCollection> hElectrons;
  iEvent.getByToken(electronToken_, hElectrons);

  // Get the beamspot
  edm::Handle<reco::BeamSpot> beamSpotHandle;
  iEvent.getByToken(beamLineToken_, beamSpotHandle);

  // prepare the MC matching
  std::vector<edm::Handle<edm::Association<reco::GenParticleCollection> > >genMatches(genMatchTokens_.size());
  if (addGenMatch_) {
    for (size_t j = 0, nd = genMatchTokens_.size(); j < nd; ++j) {
      iEvent.getByToken(genMatchTokens_[j], genMatches[j]);
    }
  }

  if (isolator_.enabled()) isolator_.beginEvent(iEvent,iSetup);

  if (efficiencyLoader_.enabled()) efficiencyLoader_.newEvent(iEvent);
  if (resolutionLoader_.enabled()) resolutionLoader_.newEvent(iEvent, iSetup);

  IsoDepositMaps deposits(isoDepositTokens_.size());
  for (size_t j = 0, nd = isoDepositTokens_.size(); j < nd; ++j) {
    iEvent.getByToken(isoDepositTokens_[j], deposits[j]);
  }

  IsolationValueMaps isolationValues(isolationValueTokens_.size());
  for (size_t j = 0; j<isolationValueTokens_.size(); ++j) {
    iEvent.getByToken(isolationValueTokens_[j], isolationValues[j]);
  }


  // prepare ID extraction
  std::vector<edm::Handle<edm::ValueMap<Bool_t> > > idhandles;
  std::vector<pat::Photon::IdPair>               ids;
  if (addPhotonID_) {
    idhandles.resize(photIDSrcs_.size());
    ids.resize(photIDSrcs_.size());
    for (size_t i = 0; i < photIDSrcs_.size(); ++i) {
      iEvent.getByToken(photIDTokens_[i], idhandles[i]);
      ids[i].first = photIDSrcs_[i].first;
    }
  }

  // loop over photons
  std::vector<Photon> * PATPhotons = new std::vector<Photon>();
  for (edm::View<reco::Photon>::const_iterator itPhoton = photons->begin(); itPhoton != photons->end(); itPhoton++) {
    // construct the Photon from the ref -> save ref to original object
    unsigned int idx = itPhoton - photons->begin();
    edm::RefToBase<reco::Photon> photonRef = photons->refAt(idx);
    edm::Ptr<reco::Photon> photonPtr = photons->ptrAt(idx);
    Photon aPhoton(photonRef);
    if (embedSuperCluster_) aPhoton.embedSuperCluster();
    if (embedSeedCluster_) aPhoton.embedSeedCluster();
    if (embedBasicClusters_) aPhoton.embedBasicClusters();
    if (embedPreshowerClusters_) aPhoton.embedPreshowerClusters();

    std::vector<DetId> selectedCells;
    bool barrel = itPhoton->isEB();
    //loop over sub clusters
    if (embedBasicClusters_) {
      for (reco::CaloCluster_iterator clusIt = itPhoton->superCluster()->clustersBegin(); clusIt!=itPhoton->superCluster()->clustersEnd(); ++clusIt) {
        //get seed (max energy xtal)
        DetId seed = lazyTools.getMaximum(**clusIt).first;
        //get all xtals in 5x5 window around the seed
        std::vector<DetId> dets5x5 = (barrel) ? ecalTopology_->getSubdetectorTopology(DetId::Ecal,EcalBarrel)->getWindow(seed,5,5):
      ecalTopology_->getSubdetectorTopology(DetId::Ecal,EcalEndcap)->getWindow(seed,5,5);
        selectedCells.insert(selectedCells.end(), dets5x5.begin(), dets5x5.end());

        //get all xtals belonging to cluster
        for (const std::pair<DetId, float> &hit : (*clusIt)->hitsAndFractions()) {
          selectedCells.push_back(hit.first);
        }
      }
    }

    //remove duplicates
    std::sort(selectedCells.begin(),selectedCells.end());
    std::unique(selectedCells.begin(),selectedCells.end());

    // Retrieve the corresponding RecHits

    edm::Handle< EcalRecHitCollection > rechitsH ;
    float cryPhi, cryEta, thetatilt, phitilt;
    int ieta, iphi;

    switch( photonRef->superCluster()->seed()->hitsAndFractions().at(0).first.subdetId() ) {
    case EcalBarrel:
      {
        iEvent.getByToken(reducedBarrelRecHitCollectionToken_,rechitsH);
        ecl_.localCoordsEB( *photonRef->superCluster()->seed(), *ecalGeometry_, cryEta, cryPhi, ieta, iphi, thetatilt, phitilt);
      }
      break;
    case EcalEndcap:
      {
        iEvent.getByToken(reducedEndcapRecHitCollectionToken_,rechitsH);
      }
      break;
    default:
     edm::LogError("PFECALSuperClusterProducer::calculateRegressedEnergy") << "Supercluster seed is either EB nor EE!" << std::endl;
    }

    EcalRecHitCollection selectedRecHits;
    const EcalRecHitCollection *recHits = rechitsH.product();

    unsigned nSelectedCells = selectedCells.size();
    for (unsigned icell = 0 ; icell < nSelectedCells ; ++icell) {
      EcalRecHitCollection::const_iterator  it = recHits->find( selectedCells[icell] );
      if ( it != recHits->end() ) {
        selectedRecHits.push_back(*it);
      }
    }
    selectedRecHits.sort();
    if (embedRecHits_) aPhoton.embedRecHits(& selectedRecHits);

    // store the match to the generated final state muons
    if (addGenMatch_) {
      for(size_t i = 0, n = genMatches.size(); i < n; ++i) {
          reco::GenParticleRef genPhoton = (*genMatches[i])[photonRef];
          aPhoton.addGenParticleRef(genPhoton);
      }
      if (embedGenMatch_) aPhoton.embedGenParticle();
    }

    if (efficiencyLoader_.enabled()) {
        efficiencyLoader_.setEfficiencies( aPhoton, photonRef );
    }

    if (resolutionLoader_.enabled()) {
        resolutionLoader_.setResolutions(aPhoton);
    }

    // here comes the extra functionality
    if (isolator_.enabled()) {
        isolator_.fill(*photons, idx, isolatorTmpStorage_);
        typedef pat::helper::MultiIsolator::IsolationValuePairs IsolationValuePairs;
        // better to loop backwards, so the vector is resized less times
        for (IsolationValuePairs::const_reverse_iterator it = isolatorTmpStorage_.rbegin(), ed = isolatorTmpStorage_.rend(); it != ed; ++it) {
            aPhoton.setIsolation(it->first, it->second);
        }
    }

    for (size_t j = 0, nd = deposits.size(); j < nd; ++j) {
        aPhoton.setIsoDeposit(isoDepositLabels_[j].first, (*deposits[j])[photonRef]);
    }

    for (size_t j = 0; j<isolationValues.size(); ++j) {
        aPhoton.setIsolation(isolationValueLabels_[j].first,(*isolationValues[j])[photonRef]);
    }

    // add photon ID info
    if (addPhotonID_) {
      for (size_t i = 0; i < photIDSrcs_.size(); ++i) {
	ids[i].second = (*idhandles[i])[photonRef];
      }
      aPhoton.setPhotonIDs(ids);
    }

    if ( useUserData_ ) {
      userDataHelper_.add( aPhoton, iEvent, iSetup );
    }

    // set conversion veto selection
    bool passelectronveto = false;
    if( hConversions.isValid()){
    // this is recommended method
      passelectronveto = !ConversionTools::hasMatchedPromptElectron(photonRef->superCluster(), hElectrons, hConversions, beamSpotHandle->position());
    }
    aPhoton.setPassElectronVeto( passelectronveto );

    // set electron veto using pixel seed (not recommended but many analysis groups are still using since it is powerful method to remove electrons)
    aPhoton.setHasPixelSeed( photonRef->hasPixelSeed() );

    // set seed energy
    aPhoton.setSeedEnergy( photonRef->superCluster()->seed()->energy() );

    // prepare input variables for regression energy correction
    float maxDR=999., maxDRDPhi=999., maxDRDEta=999., maxDRRawEnergy=0.;
    float subClusRawE[3], subClusDPhi[3], subClusDEta[3];
    memset(subClusRawE,0,3*sizeof(float));
    memset(subClusDPhi,0,3*sizeof(float));
    memset(subClusDEta,0,3*sizeof(float));
    size_t iclus=0;
    for( auto clus = photonRef->superCluster()->clustersBegin()+1; clus != photonRef->superCluster()->clustersEnd(); ++clus ) {
      const float this_deta = (*clus)->eta() - photonRef->superCluster()->seed()->eta();
      const float this_dphi = TVector2::Phi_mpi_pi((*clus)->phi() - photonRef->superCluster()->seed()->phi());
      const float this_dr = std::hypot(this_deta,this_dphi);
      if(this_dr > maxDR || maxDR == 999.0f) {
        maxDR = this_dr;
        maxDRDEta = this_deta;
        maxDRDPhi = this_dphi;
        maxDRRawEnergy = (*clus)->energy();
      }
      if( iclus++ < 3 ) {
        subClusRawE[iclus] = (*clus)->energy();
        subClusDEta[iclus] = this_deta;
        subClusDPhi[iclus] = this_dphi;
      }
    }

    const float eMax = EcalClusterTools::eMax( *photonRef->superCluster()->seed(), &*rechitsH );
    const float e2nd = EcalClusterTools::e2nd( *photonRef->superCluster()->seed(), &*rechitsH );
    const float e3x3 = EcalClusterTools::e3x3( *photonRef->superCluster()->seed(), &*rechitsH, ecalTopology_ );
    const float eTop = EcalClusterTools::eTop( *photonRef->superCluster()->seed(), &*rechitsH, ecalTopology_ );
    const float eBottom = EcalClusterTools::eBottom( *photonRef->superCluster()->seed(), &*rechitsH, ecalTopology_ );
    const float eLeft = EcalClusterTools::eLeft( *photonRef->superCluster()->seed(), &*rechitsH, ecalTopology_ );
    const float eRight = EcalClusterTools::eRight( *photonRef->superCluster()->seed(), &*rechitsH, ecalTopology_ );
    std::vector<float> vCov = EcalClusterTools::localCovariances( *photonRef->superCluster()->seed(), &*rechitsH, ecalTopology_ );
    const float see = (isnan(vCov[0]) ? 0. : sqrt(vCov[0]));
    const float spp = (isnan(vCov[2]) ? 0. : sqrt(vCov[2]));
    const float sep = vCov[1];

    // set input variables for regression energy correction
    aPhoton.setEMax( eMax );
    aPhoton.setE2nd( e2nd );
    aPhoton.setE3x3( e3x3 );
    aPhoton.setETop( eTop );
    aPhoton.setEBottom( eBottom );
    aPhoton.setELeft( eLeft );
    aPhoton.setERight( eRight );
    aPhoton.setSee( see );
    aPhoton.setSpp( spp );
    aPhoton.setSep( sep );

    aPhoton.setMaxDR( maxDR );
    aPhoton.setMaxDRDPhi( maxDRDPhi );
    aPhoton.setMaxDRDEta( maxDRDEta );
    aPhoton.setMaxDRRawEnergy( maxDRRawEnergy );
    aPhoton.setSubClusRawE1( subClusRawE[0] );
    aPhoton.setSubClusRawE2( subClusRawE[1] );
    aPhoton.setSubClusRawE3( subClusRawE[2] );
    aPhoton.setSubClusDPhi1( subClusDPhi[0] );
    aPhoton.setSubClusDPhi2( subClusDPhi[1] );
    aPhoton.setSubClusDPhi3( subClusDPhi[2] );
    aPhoton.setSubClusDEta1( subClusDEta[0] );
    aPhoton.setSubClusDEta2( subClusDEta[1] );
    aPhoton.setSubClusDEta3( subClusDEta[2] );

    aPhoton.setCryPhi( cryPhi );
    aPhoton.setCryEta( cryEta );
    aPhoton.setIEta( ieta );
    aPhoton.setIPhi( iphi );

    // add the Photon to the vector of Photons
    PATPhotons->push_back(aPhoton);
  }

  // sort Photons in ET
  std::sort(PATPhotons->begin(), PATPhotons->end(), eTComparator_);

  // put genEvt object in Event
  std::auto_ptr<std::vector<Photon> > myPhotons(PATPhotons);
  iEvent.put(myPhotons);
  if (isolator_.enabled()) isolator_.endEvent();

}

// ParameterSet description for module
void PATPhotonProducer::fillDescriptions(edm::ConfigurationDescriptions & descriptions)
{
  edm::ParameterSetDescription iDesc;
  iDesc.setComment("PAT photon producer module");

  // input source
  iDesc.add<edm::InputTag>("photonSource", edm::InputTag("no default"))->setComment("input collection");

  iDesc.add<edm::InputTag>("reducedBarrelRecHitCollection", edm::InputTag("reducedEcalRecHitsEB"));
  iDesc.add<edm::InputTag>("reducedEndcapRecHitCollection", edm::InputTag("reducedEcalRecHitsEE"));

  iDesc.add<bool>("embedSuperCluster", true)->setComment("embed external super cluster");
  iDesc.add<bool>("embedSeedCluster", true)->setComment("embed external seed cluster");
  iDesc.add<bool>("embedBasicClusters", true)->setComment("embed external basic clusters");
  iDesc.add<bool>("embedPreshowerClusters", true)->setComment("embed external preshower clusters");
  iDesc.add<bool>("embedRecHits", true)->setComment("embed external RecHits");

  iDesc.add<edm::InputTag>("electronSource", edm::InputTag("no default"))->setComment("input collection");

  // MC matching configurables
  iDesc.add<bool>("addGenMatch", true)->setComment("add MC matching");
  iDesc.add<bool>("embedGenMatch", false)->setComment("embed MC matched MC information");
  std::vector<edm::InputTag> emptySourceVector;
  iDesc.addNode( edm::ParameterDescription<edm::InputTag>("genParticleMatch", edm::InputTag(), true) xor
                 edm::ParameterDescription<std::vector<edm::InputTag> >("genParticleMatch", emptySourceVector, true)
	       )->setComment("input with MC match information");

  pat::helper::KinResolutionsLoader::fillDescription(iDesc);

  // photon ID configurables
  iDesc.add<bool>("addPhotonID",true)->setComment("add photon ID variables");
  edm::ParameterSetDescription photonIDSourcesPSet;
  photonIDSourcesPSet.setAllowAnything();
  iDesc.addNode( edm::ParameterDescription<edm::InputTag>("photonIDSource", edm::InputTag(), true) xor
                 edm::ParameterDescription<edm::ParameterSetDescription>("photonIDSources", photonIDSourcesPSet, true)
                 )->setComment("input with photon ID variables");

  // IsoDeposit configurables
  edm::ParameterSetDescription isoDepositsPSet;
  isoDepositsPSet.addOptional<edm::InputTag>("tracker");
  isoDepositsPSet.addOptional<edm::InputTag>("ecal");
  isoDepositsPSet.addOptional<edm::InputTag>("hcal");
  isoDepositsPSet.addOptional<edm::InputTag>("pfAllParticles");
  isoDepositsPSet.addOptional<edm::InputTag>("pfChargedHadrons");
  isoDepositsPSet.addOptional<edm::InputTag>("pfChargedAll");
  isoDepositsPSet.addOptional<edm::InputTag>("pfPUChargedHadrons");
  isoDepositsPSet.addOptional<edm::InputTag>("pfNeutralHadrons");
  isoDepositsPSet.addOptional<edm::InputTag>("pfPhotons");
  isoDepositsPSet.addOptional<std::vector<edm::InputTag> >("user");
  iDesc.addOptional("isoDeposits", isoDepositsPSet);

  // isolation values configurables
  edm::ParameterSetDescription isolationValuesPSet;
  isolationValuesPSet.addOptional<edm::InputTag>("tracker");
  isolationValuesPSet.addOptional<edm::InputTag>("ecal");
  isolationValuesPSet.addOptional<edm::InputTag>("hcal");
  isolationValuesPSet.addOptional<edm::InputTag>("pfAllParticles");
  isolationValuesPSet.addOptional<edm::InputTag>("pfChargedHadrons");
  isolationValuesPSet.addOptional<edm::InputTag>("pfChargedAll");
  isolationValuesPSet.addOptional<edm::InputTag>("pfPUChargedHadrons");
  isolationValuesPSet.addOptional<edm::InputTag>("pfNeutralHadrons");
  isolationValuesPSet.addOptional<edm::InputTag>("pfPhotons");
  isolationValuesPSet.addOptional<std::vector<edm::InputTag> >("user");
  iDesc.addOptional("isolationValues", isolationValuesPSet);

  // Efficiency configurables
  edm::ParameterSetDescription efficienciesPSet;
  efficienciesPSet.setAllowAnything(); // TODO: the pat helper needs to implement a description.
  iDesc.add("efficiencies", efficienciesPSet);
  iDesc.add<bool>("addEfficiencies", false);

  // Check to see if the user wants to add user data
  edm::ParameterSetDescription userDataPSet;
  PATUserDataHelper<Photon>::fillDescription(userDataPSet);
  iDesc.addOptional("userData", userDataPSet);

  edm::ParameterSetDescription isolationPSet;
  isolationPSet.setAllowAnything(); // TODO: the pat helper needs to implement a description.
  iDesc.add("userIsolation", isolationPSet);

  iDesc.addNode( edm::ParameterDescription<edm::InputTag>("beamLineSrc", edm::InputTag(), true)
                 )->setComment("input with high level selection");

  descriptions.add("PATPhotonProducer", iDesc);

}

#include "FWCore/Framework/interface/MakerMacros.h"

DEFINE_FWK_MODULE(PATPhotonProducer);
