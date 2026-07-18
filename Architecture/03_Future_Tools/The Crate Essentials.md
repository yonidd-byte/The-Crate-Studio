Feature Specification: "The Crate Essentials" (Native DSP Arsenal)
1. החזון והפילוסופיה (The Thesis)
הכוח האמיתי של אבלטון לא טמון בכך שיש לה "את הקומפרסור הכי טוב בעולם", אלא בכך שהקומפרסור שלה נטען באפס זמן, תופס מעט מאוד שטח מסך, צורך 0.1% מעבד, ואין לו לייטנסי. זה מאפשר למפיקים לייצר שרשראות סאונד-דיזיין מורכבות (15-20 אפקטים לערוץ) בלי לחשוב פעמיים. The Crate Essentials היא חבילת ה-Built-in FX של התוכנה. היא תחליף לחלוטין את הצורך בחבילות צד-שלישי בסיסיות (כמו Kilohearts Essentials), ותבטיח שהמשתמש מקבל את כל צרכי המיקס והעיצוב שלו ישירות בתוך ה-Device Chain הנייטיב שלנו.
2. חוקי הברזל של הפיתוח (Engineering Invariants)
כדי לעמוד בסטנדרט של "0% מעבד ו-0 לייטנסי", כל פלאגין בחבילה מחויב לשלושת החוקים הבאים:
Zero Latency (אפס עכבה): אין שימוש באלגוריתמים מסוג Lookahead (הסתכלות לעתיד) שמעכבים את בלוק האודיו, ואין שימוש ב-FFT (התמרת פורייה) שמצריך אגירת דגימות. הכל מתבצע ברמת הדגימה הבודדת (Sample-by-Sample) או בבלוקים מינימליים.
SIMD Vectorization: כל מתמטיקת ה-DSP תיכתב דרך מודול ה-juce::dsp שעושה אופטימיזציה ישירה לפקודות מעבד (AVX באינטל, NEON באפל סיליקון).
Headless GUI: אין חלונות צפים. כל אפקט הוא בלוק מאוזן שנדחף לתוך שרשור ה-Device Chain התחתון. כפתורים וסליידרים תמיד ייראו זהים (Flat Design) ויירשו מאותו LookAndFeel בסיסי.
3. ארסנל הכלים (The DSP Modules)
א. קטגוריית Dynamics (עיצוב תנודות ווליום)
Crate Comp (מקביל ל-Ableton Compressor / Kilohearts Comp):
טכנולוגיה: מד זיהוי עקומת RMS/Peak עם מנגנון Envelope (Attack/Release).
פיצ'רים חובה: כניסת Sidechain (חיבור ישיר מתוך הראוטינג של ה-Mixer), כפתור Dry/Wet לקומפרסיה מקבילה.
Crate OTT (מקביל ל-Ableton Multiband / Xfer OTT):
טכנולוגיה: שימוש ב-juce::dsp::LinkwitzRileyFilter כדי לפצל את האות ל-3 תדרים (Low, Mid, High) באפס היפוכי פאזה. בכל ערוץ יושב קומפרסור כפול (Upward + Downward).
פיצ'רים חובה: סליידרים של Depth (כמות האפקט) ו-Time (מהירות).
Crate Gate (מקביל ל-Ableton Gate):
טכנולוגיה: גלאי ווליום פשוט שסוגר את מעבר האות מתחת ל-Threshold.
פיצ'רים חובה: פונקציית "Flip" (מנחית סאונד מעל ה-Threshold במקום מתחתיו).
ב. קטגוריית EQ & Filtering (עיצוב תדרים)
Crate EQ8 (מקביל ל-Ableton EQ Eight / Kilohearts 3-Band):
טכנולוגיה: 8 להקות של juce::dsp::IIR::Filter. זה אלגוריתם מוכן ומאוד יעיל של JUCE.
פיצ'רים חובה: תצוגת ספקטרום מינימליסטית ברקע, אפשרות עריכת תדרים (Bell, Shelf, Cut).
Crate Filter (מקביל ל-Ableton Auto Filter):
טכנולוגיה: פילטר מסוג State Variable Filter (juce::dsp::StateVariableTPTFilter) המאפשר מודולציות מהירות מאוד על ה-Cutoff מבלי לייצר "קליקים" (Zippers) בסאונד.
פיצ'רים חובה: LFO מובנה שמזיז את הפילטר.
ג. קטגוריית Distortion & Color (השחתה וצבע)
Crate Drive (מקביל ל-Ableton Saturator / Overdrive):
טכנולוגיה: מנוע Waveshaper. משתמש בנוסחה מתמטית פשוטה (כמו std::tanh) כדי לעקם את האות ולייצר הרמוניות. יעילות CPU מושלמת (פעולת כפל בלבד).
פיצ'רים חובה: בחירת עקומות (Soft Clip, Hard Clip, Foldback).
Crate Erosion (מקביל ל-Ableton Erosion):
טכנולוגיה: חולץ סינוס או מחולל רעש לבן שמבצע מודולציה ל-Bandpass פילטר (Amplitude Modulation).
פיצ'רים חובה: בחירה בין Noise ל-Sine ל-Wide.
Crate Crush (מקביל ל-Ableton Redux / Kilohearts Bitcrush):
טכנולוגיה: מנוע Sample Rate Reduction (הורדת רזולוציית דגימה על ידי דילוג על דגימות) ו-Bit Reduction (עיגול ה-Amplitude לערכים קבועים). מתמטיקה טהורה וקלה.
ד. קטגוריית Modulation & Space (תנועה ומרחב)
Crate Chorus / Flanger / Phaser:
טכנולוגיה: שלושתם יושבים על אותו מנוע בדיוק – קו השהייה (Delay Line) קצר מאוד (1-20ms) שזז למעלה ולמטה באמצעות LFO. (Phaser משתמש ב-Allpass Filters במקום Delay). juce::dsp::DelayLine יסגור את הפינה.
Crate Delay (מקביל ל-Ableton Echo/Ping Pong):
טכנולוגיה: באפר (Buffer) ששומר אודיו ומנגן אותו בחזרה עם פידבק.
פיצ'רים חובה: סנכרון לטמפו הפרויקט (1/4, 1/8D) ופילטר בתוך לולאת הפידבק (שכל הד הופך לכהה יותר).
Crate Room (מקביל ל-Ableton Reverb):
טכנולוגיה: Algorithmic Reverb (מבוסס רשת של פילטרים מסוג Comb ו-Allpass). לא נשתמש ב-Convolution (IR) כי הוא צורך לייטנסי ומעבד.
הערה טכנית: ניעזר ב-juce::dsp::Reverb או באלגוריתם Open Source פתוח (MIT License) כמו Freeverb, המעניק סאונד חלק במינימום משאבים.
ה. קטגוריית Utilities (כלי עבודה בסיסיים)
Crate Utility (מקביל ל-Ableton Utility / Kilohearts Gain):
טכנולוגיה: פעולות כפל של דגימות (Gain), הפיכת פאזה מילולית (sample * -1), ומתמטיקת Mid/Side פשוטה לבקרת רוחב הסטריאו.
Crate Pitch (מקביל ל-Kilohearts Pitch Shifter):
טכנולוגיה: אלגוריתם Grain-delay. (זה הפלאגין היחיד שיכול להוסיף לייטנסי קליל מטבעו, אז נפעיל אותו רק כשחייבים).
4. מודל העבודה (The Development Workflow)
במקום לעבוד חצי שנה על יצירת 20 חלונות פלאגינים שונים, אנחנו מאמצים ארכיטקטורת Core DSP & Wrapper:
The Core: כל האלגוריתמים ישבו בתיקיית Source/DSP/Essentials. הם יהיו נטולי UI לחלוטין (רק מתמטיקה).
The Device Wrapper: כתבנו את ה-DeviceChain. נבנה Component גנרי שיודע לקבל אובייקט DSP ולצייר את הפרמטרים שלו ככפתורים וסליידרים שטוחים בתוך שרשור הכלים של ה-Tracktion Engine.
The Rollout: הוספת פלאגין חדש למערכת תיקח יום עבודה אחד – אנחנו רק מחברים מתמטיקה מהמוכן של JUCE ל-Wrapper שבנינו.

