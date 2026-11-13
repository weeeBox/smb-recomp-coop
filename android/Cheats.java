// The cheats screen: Android's own settings look (PreferenceActivity), opened from the system's
// App info page ("Additional settings in the app", or its gear icon). Choices are kept in the
// app's SharedPreferences and, on leaving the screen, written to files/cheats.txt as
// "key=value" words, which the game (cheats.c) reads each time it comes back to the front.
// Level select takes effect at the next new game; the rest at once.
package local.recomp;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.preference.ListPreference;
import android.preference.PreferenceActivity;
import android.preference.PreferenceScreen;
import android.preference.SwitchPreference;
import java.io.FileOutputStream;

public class Cheats extends PreferenceActivity {
  @Override protected void onCreate(Bundle b) {
    super.onCreate(b);
    PreferenceScreen s = getPreferenceManager().createPreferenceScreen(this);
    s.addPreference(toggle("lives", "Infinite lives", "Never fewer than 3 lives: no game over"));
    String[] levels = new String[32];
    for (int i = 0; i < 32; i++) levels[i] = (i / 4 + 1) + "-" + (i % 4 + 1);
    ListPreference l = new ListPreference(this);
    l.setKey("level"); l.setTitle("Start at level"); l.setSummary("%s (from the next new game)");
    l.setEntries(levels); l.setEntryValues(levels); l.setDefaultValue("1-1");
    s.addPreference(l);
    s.addPreference(toggle("time", "Stop the clock", "The timer never runs down"));
    s.addPreference(toggle("fire", "Fire power", "Start every life and area as Fire Mario"));
    setPreferenceScreen(s);
  }

  private SwitchPreference toggle(String key, String title, String summary) {
    SwitchPreference p = new SwitchPreference(this);
    p.setKey(key); p.setTitle(title); p.setSummary(summary);
    return p;
  }

  @Override protected void onPause() {
    super.onPause();
    SharedPreferences p = getPreferenceManager().getSharedPreferences();
    String[] wl = p.getString("level", "1-1").split("-");
    String out = "lives=" + (p.getBoolean("lives", false) ? 1 : 0) + " world=" + wl[0] + " level=" + wl[1]
        + " time=" + (p.getBoolean("time", false) ? 1 : 0) + " fire=" + (p.getBoolean("fire", false) ? 1 : 0) + "\n";
    try (FileOutputStream f = new FileOutputStream(getFilesDir() + "/cheats.txt")) {
      f.write(out.getBytes());
    } catch (Exception e) {
      android.util.Log.w("recomp", "cheats.txt: " + e);
    }
  }
}
