from datetime import datetime
from io import BytesIO
import qrcode
from flask import Flask, jsonify, redirect, render_template, request, send_file, url_for
from flask_sqlalchemy import SQLAlchemy

app = Flask(__name__)
app.config['SQLALCHEMY_DATABASE_URI'] = 'sqlite:///iot_project.db'
app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False
db = SQLAlchemy(app)

def now(): return datetime.now()

class Shipment(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    code = db.Column(db.String(20), unique=True, nullable=False)
    product = db.Column(db.String(100), nullable=False)
    origin = db.Column(db.String(100), nullable=False)
    destination = db.Column(db.String(100), nullable=False)
    route = db.Column(db.String(20), nullable=False)
    device_id = db.Column(db.String(30), default='TRUCK_01')
    stage = db.Column(db.String(20), default='LOADING')
    active = db.Column(db.Boolean, default=False)
    status = db.Column(db.String(30), default='NOT_STARTED')
    created_at = db.Column(db.DateTime, default=now)
    started_at = db.Column(db.DateTime)
    ended_at = db.Column(db.DateTime)
    last_seen = db.Column(db.DateTime)

class Configuration(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    shipment_code = db.Column(db.String(20), unique=True, nullable=False)
    min_temp = db.Column(db.Float, default=20.0)
    max_temp = db.Column(db.Float, default=27.0)
    allowed_excursion = db.Column(db.Integer, default=20)
    moderate_impact = db.Column(db.Float, default=0.35)
    severe_impact = db.Column(db.Float, default=0.80)
    version = db.Column(db.Integer, default=1)

class Reading(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    shipment_code = db.Column(db.String(20), nullable=False)
    recorded_at = db.Column(db.DateTime, default=now)
    temperature = db.Column(db.Float)
    humidity = db.Column(db.Float)
    magnitude_g = db.Column(db.Float)
    peak_impact_g = db.Column(db.Float)
    classification = db.Column(db.String(30))

class Event(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    shipment_code = db.Column(db.String(20), nullable=False)
    recorded_at = db.Column(db.DateTime, default=now)
    stage = db.Column(db.String(20))
    event_type = db.Column(db.String(50))
    severity = db.Column(db.String(20))
    value = db.Column(db.Float)
    description = db.Column(db.String(250))

with app.app_context(): db.create_all()

def ship(code): return Shipment.query.filter_by(code=code).first_or_404()
def event(s, kind, severity, text, value=None):
    db.session.add(Event(shipment_code=s.code, stage=s.stage, event_type=kind,
                         severity=severity, description=text, value=value))

def excursion_stats(code):
    s = ship(code); c = Configuration.query.filter_by(shipment_code=code).first()
    rows = Reading.query.filter_by(shipment_code=code).order_by(Reading.recorded_at).all()
    start = None; last = None; total = 0.0; longest = 0.0
    for r in rows:
        outside = r.temperature is not None and (r.temperature < c.min_temp or r.temperature > c.max_temp)
        if outside and start is None: start = r.recorded_at
        if not outside and start is not None:
            duration = (r.recorded_at - start).total_seconds(); total += duration; longest = max(longest, duration); start = None
        last = r.recorded_at
    if start and last:
        duration = (last - start).total_seconds(); total += duration; longest = max(longest, duration)
    return total, longest

def update_status(s):
    c = Configuration.query.filter_by(shipment_code=s.code).first()
    _, longest = excursion_stats(s.code)
    severe = Event.query.filter_by(shipment_code=s.code, event_type='SEVERE_IMPACT').count()
    moderate = Event.query.filter_by(shipment_code=s.code, event_type='MODERATE_IMPACT').count()
    if longest >= c.allowed_excursion or severe >= 3: s.status = 'INTEGRITY_FAILED'
    elif longest > 0 or severe > 0 or moderate > 0: s.status = 'INSPECTION_REQUIRED'
    elif s.active or s.ended_at: s.status = 'NORMAL'

@app.route('/', methods=['GET', 'POST'])
def index():
    if request.method == 'POST':
        latest = Shipment.query.order_by(Shipment.id.desc()).first()
        code = f'S{(latest.id + 1 if latest else 1):03d}'
        s = Shipment(code=code, product=request.form['product'], origin=request.form['origin'],
                     destination=request.form['destination'], route=request.form['route'],
                     device_id=request.form.get('device_id', 'TRUCK_01'))
        c = Configuration(shipment_code=code, min_temp=float(request.form['min_temp']),
                          max_temp=float(request.form['max_temp']),
                          allowed_excursion=int(request.form['allowed_excursion']),
                          moderate_impact=float(request.form['moderate_impact']),
                          severe_impact=float(request.form['severe_impact']))
        db.session.add_all([s, c]); db.session.commit()
        return redirect(url_for('dashboard', code=code))
    return render_template('index.html', shipments=Shipment.query.order_by(Shipment.id.desc()).all())

@app.route('/dashboard/<code>', methods=['GET', 'POST'])
def dashboard(code):
    s = ship(code); c = Configuration.query.filter_by(shipment_code=code).first()
    if request.method == 'POST':
        action = request.form['action']
        if action == 'start': s.active = True; s.started_at = now(); s.status = 'NORMAL'; event(s, 'JOURNEY_STARTED', 'INFO', 'Journey monitoring started')
        elif action == 'end': s.active = False; s.ended_at = now(); event(s, 'JOURNEY_ENDED', 'INFO', 'Journey monitoring ended')
        elif action == 'stage': s.stage = request.form['stage']; event(s, 'STAGE_CHANGED', 'INFO', f'Stage changed to {s.stage}')
        elif action == 'config':
            c.min_temp=float(request.form['min_temp']); c.max_temp=float(request.form['max_temp']); c.allowed_excursion=int(request.form['allowed_excursion']); c.moderate_impact=float(request.form['moderate_impact']); c.severe_impact=float(request.form['severe_impact']); c.version += 1
            event(s, 'CONFIG_UPDATED', 'INFO', 'Configuration updated by staff')
        update_status(s); db.session.commit(); return redirect(url_for('dashboard', code=code))
    total, longest = excursion_stats(code)
    latest = Reading.query.filter_by(shipment_code=code).order_by(Reading.recorded_at.desc()).first()
    readings = Reading.query.filter_by(shipment_code=code).order_by(Reading.recorded_at.desc()).limit(50).all()
    events = Event.query.filter_by(shipment_code=code).order_by(Event.recorded_at.desc()).limit(50).all()
    return render_template('dashboard.html', s=s, c=c, latest=latest, readings=readings, events=events, total=total, longest=longest)

@app.route('/track/<code>')
def track(code):
    s=ship(code); total,longest=excursion_stats(code)
    latest=Reading.query.filter_by(shipment_code=code).order_by(Reading.recorded_at.desc()).first()
    events=Event.query.filter_by(shipment_code=code).order_by(Event.recorded_at.desc()).all()
    return render_template('track.html', s=s, latest=latest, events=events, total=total, longest=longest)

@app.route('/qr/<code>')
def qr(code):
    ship(code); image=qrcode.make(url_for('track', code=code, _external=True)); out=BytesIO(); image.save(out, format='PNG'); out.seek(0); return send_file(out, mimetype='image/png')

@app.route('/analysis')
def analysis():
    result=[]
    for route in ['ROUTE_A','ROUTE_B','ROUTE_C']:
        ss=Shipment.query.filter_by(route=route).all(); codes=[s.code for s in ss]
        impacts=Event.query.filter(Event.shipment_code.in_(codes), Event.event_type.in_(['MODERATE_IMPACT','SEVERE_IMPACT'])).count() if codes else 0
        result.append({'route':route,'count':len(ss),'normal':sum(x.status=='NORMAL' for x in ss),'inspect':sum(x.status=='INSPECTION_REQUIRED' for x in ss),'failed':sum(x.status=='INTEGRITY_FAILED' for x in ss),'impacts':impacts})
    return render_template('analysis.html', routes=result)

@app.get('/api/config/<device_id>')
def config_api(device_id):
    s=Shipment.query.filter_by(device_id=device_id, active=True).order_by(Shipment.started_at.desc()).first()
    if not s: return jsonify(journey_active=False)
    c=Configuration.query.filter_by(shipment_code=s.code).first()
    return jsonify(journey_active=True, shipment_code=s.code, stage=s.stage,
                   minimum_temperature=c.min_temp, maximum_temperature=c.max_temp,
                   allowed_excursion_seconds=c.allowed_excursion,
                   moderate_impact_threshold=c.moderate_impact,
                   severe_impact_threshold=c.severe_impact, configuration_version=c.version)

@app.post('/api/telemetry')
def telemetry():
    data=request.get_json(force=True); s=ship(data['shipment_code'])
    if not s.active: return jsonify(error='journey not active'),409
    s.last_seen=now()
    if data['kind']=='summary':
        r=Reading(shipment_code=s.code,temperature=float(data['temperature']),humidity=float(data['humidity']),magnitude_g=float(data['magnitude_g']),peak_impact_g=float(data['peak_impact_g']),classification=data.get('classification','NORMAL'))
        db.session.add(r)
        c=Configuration.query.filter_by(shipment_code=s.code).first()
        if r.temperature<c.min_temp or r.temperature>c.max_temp: event(s,'TEMP_OUT_OF_RANGE','WARNING',f'Temperature {r.temperature:.2f} C outside configured range',r.temperature)
    else:
        kind=data['event_type']; value=float(data.get('impact_g',0)); event(s,kind,'CRITICAL' if kind=='SEVERE_IMPACT' else 'WARNING',kind.replace('_',' ').title(),value)
    db.session.flush(); update_status(s); db.session.commit()
    return jsonify(ok=True,status=s.status)

@app.get('/api/live/<code>')
def live(code):
    s=ship(code); r=Reading.query.filter_by(shipment_code=code).order_by(Reading.recorded_at.desc()).first(); total,longest=excursion_stats(code)
    return jsonify(status=s.status,stage=s.stage,active=s.active,last_seen=s.last_seen.isoformat() if s.last_seen else None,
                   temperature=r.temperature if r else None,humidity=r.humidity if r else None,magnitude_g=r.magnitude_g if r else None,peak_impact_g=r.peak_impact_g if r else None,total_excursion=total,longest_excursion=longest)

if __name__ == '__main__': app.run(host='0.0.0.0', port=5000, debug=False)
