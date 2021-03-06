#include "flockengine.h"

// TODO clean this up.
#include <Eigen/Core>

#include <QtCore/QDebug>
#include <QtCore/QTimer>

#include <QtWidgets/QApplication>

#include <QtGui/QBrush>
#include <QtGui/QKeyEvent>
#include <QtGui/QPen>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>

#include <QtConcurrent/QtConcurrentMap>

#include <cstdlib>
#include <ctime>

#include "blast.h"
#include "flocker.h"
#include "predator.h"
#include "target.h"

namespace {
// See http://www.musicdsp.org/showone.php?id=222 for more info
inline float fastexp5(float x) {
    return (120+x*(120+x*(60+x*(20+x*(5+x)))))*0.0083333333f;
}

inline double V_morse(const double r) {
  const double depth = 1.00;
  const double rad   = 0.10;
  const double alpha = 2.00;

  const double tmpTerm = ( 1 - fastexp5( -alpha * (r - rad) ) );
  double V = depth * tmpTerm * tmpTerm;

  return V - depth;
}

// Numerical derivatives
inline double V_morse_ND(const double r) {
  const double delta = r * 1e-5;
  const double v1 = V_morse(r - delta);
  const double v2 = V_morse(r + delta);
  return (v2 - v1) / (delta + delta);
}

// Weight for each competing force
static double diffPotWeight  = 0.10; // morse potential, all type()s
static double samePotWeight  = 0.20; // morse potential, same type()
static double alignWeight    = 0.40; // Align to average neighbor heading
static double predatorWeight = 1.20; // 1/r^2 attraction/repulsion to all predators
static double targetWeight   = 1.20; // 1/r^2 attraction to all targets
static double clickWeight    = 2.00; // 1/r^2 attraction to clicked point
static double boundaryWeight = 1.10; // boundary evasion
// newDirection = (oldDirection + (factor) * maxTurn * force).normalized()
static const double maxTurn = 0.20;
// velocity *= 1.0 + speedupFactor * direction.dot(goalForce)
static const double speedupFactor = 0.075;
// Kill radius for if predators catch flockers:
static const double killRadius = 0.020;
// Fraction from boundary to begin repulsion
// rmax in force = ((rmax - r) / rmax) * norm (m @ 0, 0 @ rmax
static const double boundaryRMax  = 0.25;
static const double boundaryRMax2 = boundaryRMax * boundaryRMax;

// (rmax - r) / rmax

// Normalize force weights:
static double invWeightSum = 1.0 / (diffPotWeight + samePotWeight + alignWeight +
                                    predatorWeight + targetWeight + boundaryWeight);

// TODO These vars could just be const...
static void initWorker()
{
  static bool inited = false;
  if (inited) {
    diffPotWeight  *= invWeightSum;
    samePotWeight  *= invWeightSum;
    alignWeight    *= invWeightSum;
    predatorWeight *= invWeightSum;
    targetWeight   *= invWeightSum;
    boundaryWeight *= invWeightSum;
  }
}
} // end anon namespace

FlockEngine::FlockEngine(QObject *parent)
  : QObject(parent),
    m_useForceTarget(false),
    m_entityIdHead(0),
    m_numFlockers(500),
    m_numFlockerTypes(12),
    m_numPredators(30),
    m_numPredatorTypes(3),
    m_numTargetsPerFlockerType(3),
    m_stepSize(1.),
    m_initialSpeed(0.0050),
    m_minSpeed(    0.0015),
    m_maxSpeed(    0.0075)
{
  initWorker();
  this->initializeFlockers();
  this->initializePredators();
  this->initializeTargets();

  // Initialize RNG
  srand(time(NULL));
}

FlockEngine::~FlockEngine()
{
  this->cleanupFlockers();
  this->cleanupPredators();
  this->cleanupTargets();
}

const Eigen::Vector3d &FlockEngine::forceTarget() const
{
  return m_forceTarget;
}

void FlockEngine::setForceTarget(const Eigen::Vector3d &v)
{
  m_forceTarget = v;
}

bool FlockEngine::useForceTarget() const
{
  return m_useForceTarget;
}

void FlockEngine::setUseForceTarget(bool b)
{
  m_useForceTarget = b;
}

struct FlockEngine::TakeStepResult
{
  Eigen::Vector3d newDirection;
  double newVelocity;
  const Target *deadTarget;
  const Flocker *deadFlocker;
};

struct FlockEngine::TakeStepFunctor
{
  TakeStepFunctor(FlockEngine &w) : engine(w) {}
  FlockEngine &engine;

  typedef TakeStepResult result_type;

  FlockEngine::TakeStepResult operator()(const Flocker *f)
  {
    return engine.takeStepWorker(f);
  }
};

namespace {
bool isNan(double d)
{
  return d != d;
}
} // end anon namespace

FlockEngine::TakeStepResult
FlockEngine::takeStepWorker(const Flocker *f_i) const
{
  const bool pred_i = f_i->eType() == Entity::PredatorEntity;

  TakeStepResult result;
  result.deadTarget = NULL;
  result.deadFlocker = NULL;

  Eigen::Vector3d samePotForce(0., 0., 0.);
  Eigen::Vector3d diffPotForce(0., 0., 0.);
  Eigen::Vector3d alignForce(0., 0., 0.);
  Eigen::Vector3d predatorForce(0., 0., 0.);
  Eigen::Vector3d targetForce(0., 0., 0.);
  Eigen::Vector3d boundaryForce(0., 0., 0.);
  Eigen::Vector3d r(0., 0., 0.);

  if (!m_useForceTarget) {
    // Calculate a distance-weighted average vector towards the relevant
    // targets
    if (!pred_i) {
      foreach (const Target *t, m_targets[f_i->type()]) {
        r = t->pos() - f_i->pos();
        const double rNorm = r.norm();
        if (rNorm < 0.025)
          result.deadTarget = t;
        else
          targetForce += (1.0/(rNorm*rNorm*rNorm*rNorm)) * r;
      }
    }
  }
  else {
    // Ignore targets and pull towards clicked point.
    r = m_forceTarget - f_i->pos();
    const double rNorm = r.norm();
    if (rNorm > 0.01)
      targetForce = (1.0/(rNorm*rNorm*rNorm)) * r;
  }

  // Average together V(|r_ij|) * r_ij
  foreach (const Flocker *f_j, m_flockers) {
    if (f_i == f_j) continue;
    r = f_j->pos() - f_i->pos();

    // General cutoff
    const double cutoff = pred_i ? 0.6 : 0.3;
    if (r.x() > cutoff || r.y() > cutoff || r.z() > cutoff)
      continue;

    const double rNorm = r.norm();
    const double rInvNorm = rNorm > 0.01 ? 1.0 / rNorm : 1.0;

    const bool pred_j = f_j->eType() == Entity::PredatorEntity;

    // Neither are predators, use morse potential
    bool bothArePredators = pred_i && pred_j;
    bool neitherArePredators = !pred_i && !pred_j;
    bool typesMatch = f_i->type() == f_j->type();

    if (neitherArePredators || (bothArePredators && typesMatch)) {
      double V = std::numeric_limits<double>::max();

      // Apply cutoff for morse interaction
      if (rNorm < 0.20) {
        V = V_morse_ND(rNorm);
        diffPotForce += (V*rInvNorm*rInvNorm) * r;
      }

      // Alignment -- steer towards the heading of nearby flockers
      if (typesMatch &&
          rNorm < 0.30 && rNorm > 0.001) {
        if (V == std::numeric_limits<double>::max()) {
          V = V_morse_ND(rNorm);
        }
        samePotForce += (V *rInvNorm*rInvNorm) * r;
        alignForce += rInvNorm * f_j->direction();
      }
    }
    else if (bothArePredators && !typesMatch) {
      r = f_i->pos() - f_j->pos();
      const double rNorm = r.norm();
      if (rNorm < 0.5) {
        const double rInvNorm = rNorm > 0.01 ? 1.0 / rNorm : 1.0;
        //              2    normalize     1/r2     vector
        predatorForce = 2. * rInvNorm * (rInvNorm * rInvNorm) * r;
      }
    }
    // One is a predator, one is not. Evade / Pursue
    else {
      const Flocker *p;
      const Flocker *f;
      if (pred_i) {
        p = f_i;
        f = f_j;
      }
      else {
        p = f_j;
        f = f_i;
      }

      // Calculate distance between them
      r = f->pos() - p->pos();
      const double rNorm = r.norm();
      const double rInvNorm = rNorm > 0.01 ? 1.0 / rNorm : 1.0;
      if (pred_j) {
        // The flocker is being updated
        if (rNorm < 0.3) {
          // Did the predator catch the flocker?
          if (rNorm < killRadius) {
            result.deadFlocker = f;
          }
          else {
            //               normalize               1/r^3               vector
            predatorForce += rInvNorm * (rInvNorm * rInvNorm * rInvNorm) * r;
          }
        }
      }
      // The predator is being updated
      else {
        // Cutoff distance for predator
        if (rNorm < 0.15) {
          //               2    normalize          1/r*2         vector
          predatorForce += 2. * rInvNorm * (rInvNorm * rInvNorm) * r;
        }
        else {
          //               normalize     1/r     vector
          predatorForce += rInvNorm * (rInvNorm) * r;
        }
      }
    }
  }

  // Repel boundaries
  const double minBound = boundaryRMax;
  const double maxBound = 1.0 - boundaryRMax;
  const Eigen::Vector3d basis[3] = { Eigen::Vector3d(1, 0, 0),
                                     Eigen::Vector3d(0, 1, 0),
                                     Eigen::Vector3d(0, 0, 1) };

  for (size_t i = 0; i < 3; ++i) {
    if (f_i->pos()[i] < minBound) {
      const double r2 = f_i->pos()[i] * f_i->pos()[i];
      boundaryForce += ((boundaryRMax2 - r2) / boundaryRMax2) * basis[i];
    }
    if (f_i->pos()[i] > maxBound) {
      const double r2 = (1.0 - f_i->pos()[i]) * (1.0 - f_i->pos()[i]);
      boundaryForce -= ((boundaryRMax2 - r2) / boundaryRMax2) * basis[i];
    }
  }

  if (!diffPotForce.isZero(0.1)) {
    diffPotForce.normalize();
  }
  if (!samePotForce.isZero(0.1)) {
    samePotForce.normalize();
  }
  if (!alignForce.isZero(0.1)) {
    alignForce.normalize();
  }
  if (!predatorForce.isZero(0.1)) {
    predatorForce.normalize();
  }
  if (!targetForce.isZero(0.1)) {
    targetForce.normalize();
  }
  // Don't normalize boundary force -- it's kept reasonable.

  // target weight changes when clicked:
  const double rTargetWeight = m_useForceTarget ? (pred_i ? -clickWeight
                                                          : clickWeight)
                                                : targetWeight;

  // Scale the force so that it will turn faster when "direction" is not
  // aligned well with force:
  Eigen::Vector3d force (samePotWeight  * samePotForce  +
                         diffPotWeight  * diffPotForce  +
                         alignWeight    * alignForce    +
                         predatorWeight * predatorForce +
                         rTargetWeight  * targetForce   +
                         boundaryWeight * boundaryForce );
  if (!force.isZero(0.1)) {
    force.normalize();
    // Calculate the rejection of force onto direction:
    force -= force.dot(f_i->direction()) * f_i->direction();
  }

  const double directionDotForce = f_i->direction().dot(force);
  const double scale ((1.0 - 0.5 * (directionDotForce + 1.0)) * maxTurn);
  result.newDirection = (f_i->direction() + scale * force).normalized();

  //dDF  :    -1     -0.5       0       0.5       1
  //scale:     0.25  ~0.19      0.125  ~0.06      0

  // Accelerate towards goal
  const double goalForce = f_i->direction().dot(0.15 * predatorForce +
                                                0.25 * targetForce +
                                                0.6 * samePotForce);
  result.newVelocity = f_i->velocity() * (1.0 + speedupFactor * goalForce);
  if (result.newVelocity < m_minSpeed)
    result.newVelocity = m_minSpeed;
  else if (result.newVelocity > m_maxSpeed)
    result.newVelocity = m_maxSpeed;

  return result;
}

double FlockEngine::stepSize() const
{
  return m_stepSize;
}

void FlockEngine::setStepSize(double size)
{
  m_stepSize = size;
}

unsigned int FlockEngine::numTargetsPerFlockerType() const
{
  return m_numTargetsPerFlockerType;
}

unsigned int FlockEngine::numFlockerTypes() const
{
  return m_numFlockerTypes;
}

bool FlockEngine::createBlasts() const
{
  return m_createBlasts;
}

void FlockEngine::setCreateBlasts(bool b)
{
  m_createBlasts = b;
}

QColor FlockEngine::typeToColor(const unsigned int type)
{
  static const unsigned int numColors = 12;
  const unsigned int colorMod = (m_numFlockerTypes < numColors)
                                ? m_numFlockerTypes : numColors;

  switch (type % colorMod)
  {
  case 0:
    return QColor(Qt::blue);
  case 1:
    return QColor(Qt::darkCyan);
  case 2:
    return QColor(Qt::green);
  case 3:
    return QColor(Qt::yellow);
  case 4:
    return QColor(Qt::white);
  case 5:
    return QColor(Qt::cyan);
  case 6:
    return QColor(Qt::magenta);
  case 7:
    return QColor(Qt::darkGray);
  case 8:
    return QColor(Qt::lightGray);
  case 9:
    return QColor(Qt::darkMagenta);
  case 10:
    return QColor(Qt::darkBlue);
  case 11:
    return QColor(Qt::darkGreen);
  default:
    qWarning() << "Unrecognized type:" << type;
    return QColor();
  }
}

namespace {
struct EntityStepFunctor
{
  double m_t;
  EntityStepFunctor(double t) : m_t(t) {}
  void operator()(Entity *e) const { e->takeStep(m_t); }
};
}

void FlockEngine::computeNextStep()
{
  Q_ASSERT(!m_future.isRunning());
  m_future = QtConcurrent::mapped(m_flockers, TakeStepFunctor(*this));
}

void FlockEngine::commitNextStep()
{
  Q_ASSERT(m_future.isStarted());
  m_future.waitForFinished();

  QVector<Flocker*> deadFlockers;
  QVector<Target*> deadTargets;

  QLinkedList<Flocker*>::iterator current = m_flockers.begin();
  foreach (const TakeStepResult &result, m_future.results()) {
    (*current)->direction() = result.newDirection;
    (*current)->velocity() = result.newVelocity;
    if (result.deadFlocker)
      deadFlockers.push_back(const_cast<Flocker*>(result.deadFlocker));
    if (result.deadTarget)
      deadTargets.push_back(const_cast<Target*>(result.deadTarget));
    ++current;
  }

  QVector<Blast*> deadBlasts;
  deadBlasts.reserve(m_blasts.size());
  foreach (Blast *b, m_blasts) {
    if (b->done())
      deadBlasts.push_back(b);
  }

  foreach (Blast *b, deadBlasts)
    removeBlast(b);

  foreach (Flocker *f, deadFlockers) {
    this->removeFlocker(f);
    if (!m_createBlasts)
      this->addBlastFromEntity(f);
  }

  foreach (Target *t, deadTargets) {
    this->addFlockerFromEntity(t);
    this->randomizeTarget(t);
  }

  QFuture<void> stepFuture = QtConcurrent::map(m_entities,
                                               EntityStepFunctor(m_stepSize));
  qApp->processEvents();
  stepFuture.waitForFinished();
}

void FlockEngine::initializeFlockers()
{
  this->cleanupFlockers();

  for (unsigned int i = 0; i < m_numFlockers; ++i) {
    this->addRandomFlocker();
  }
}

void FlockEngine::cleanupFlockers()
{
  foreach (Flocker *f, m_flockers) {
    m_entities.removeOne(f);
  }
  qDeleteAll(m_flockers);
  m_flockers.clear();
  m_predators.clear();
}

void FlockEngine::addFlockerFromEntity(const Entity *e)
{
  Flocker *newFlocker = new Flocker (m_entityIdHead++, e->type());
  newFlocker->pos() = e->pos();
  newFlocker->direction() = e->direction();
  newFlocker->velocity() = e->velocity();
  newFlocker->color() = e->color();

  m_flockers.push_back(newFlocker);
  m_entities.push_back(newFlocker);
}

void FlockEngine::addRandomFlocker()
{
  Flocker *f = new Flocker (m_entityIdHead, m_entityIdHead % m_numFlockerTypes);
  m_entityIdHead++;

  this->randomizeVector(&f->pos());

  f->direction().setRandom();
  f->direction().normalize();

  f->velocity() = m_initialSpeed;

  f->color() = this->typeToColor(f->type());

  m_flockers.push_back(f);
  m_entities.push_back(f);
}

void FlockEngine::removeFlocker(Flocker *f)
{
  m_flockers.removeOne(f);
  m_entities.removeOne(f);
  f->deleteLater();
}

void FlockEngine::initializePredators()
{
  this->cleanupPredators();

  unsigned int type = 0;
  for (unsigned int i = 0; i < m_numPredators; ++i) {
    this->addRandomPredator(type);
    if (++type == m_numPredatorTypes)
      type = 0;
  }
}

void FlockEngine::cleanupPredators()
{
  foreach (Predator *p, this->m_predators) {
    m_flockers.removeOne(p);
    m_entities.removeOne(p);
  }
  qDeleteAll(this->m_predators);
  this->m_predators.clear();
}

void FlockEngine::addRandomPredator(const unsigned int type)
{
  Predator *p = new Predator (m_entityIdHead++, type);

  this->randomizeVector(&p->pos());

  p->direction().setRandom();
  p->direction().normalize();

  p->velocity() = m_initialSpeed;

  p->color() = QColor(Qt::red);

  m_predators.push_back(p);
  m_flockers.push_back(p);
  m_entities.push_back(p);
}

void FlockEngine::removePredator(Predator *p)
{
  m_predators.removeOne(p);
  m_flockers.removeOne(p);
  m_entities.removeOne(p);
  p->deleteLater();
}

void FlockEngine::initializeTargets()
{
  this->cleanupTargets();

  for (unsigned int type = 0; type < m_numFlockerTypes; ++type) {
    for (unsigned int i = 0; i < m_numTargetsPerFlockerType; ++i) {
      this->addRandomTarget(type);
    }
  }
}

void FlockEngine::cleanupTargets()
{
  for (int i = 0; i < m_targets.size(); ++i) {
    qDeleteAll(m_targets[i]);
  }

  this->m_targets.clear();
}

void FlockEngine::addRandomTarget(const unsigned int type)
{
  Target *newTarget = new Target(m_entityIdHead++, type);

  randomizeVector(&newTarget->pos());
  newTarget->color() = this->typeToColor(type);
  newTarget->velocity() = m_minSpeed;
  randomizeVector(&newTarget->direction());
  newTarget->direction().normalize();

  if (type + 1 > static_cast<unsigned int>(m_targets.size())) {
    m_targets.resize(type + 1);
  }

  m_targets[type].push_back(newTarget);
  m_entities.push_back(newTarget);
}

void FlockEngine::removeTarget(Target *t)
{
  m_targets[t->type()].removeOne(t);
  m_entities.removeOne(t);
  t->deleteLater();
}

void FlockEngine::randomizeTarget(Target *t)
{
  this->randomizeVector(&t->pos());
}

void FlockEngine::addBlastFromEntity(const Entity *e)
{
  Blast *newBlast = new Blast (m_entityIdHead++, e->type());
  newBlast->pos() = e->pos();
  newBlast->direction() = e->direction();
  newBlast->velocity() = e->velocity();
  newBlast->color() = e->color();

  m_blasts.push_back(newBlast);
  m_entities.push_back(newBlast);
}

void FlockEngine::removeBlast(Blast *b)
{
  m_blasts.removeOne(b);
  m_entities.removeOne(b);
  b->deleteLater();
}

void FlockEngine::randomizeVector(Eigen::Vector3d *vec)
{
  static const double invRANDMAX = 1.0/static_cast<double>(RAND_MAX);
  vec->x() = rand() * invRANDMAX;
  vec->y() = rand() * invRANDMAX;
  vec->z() = rand() * invRANDMAX;
}
