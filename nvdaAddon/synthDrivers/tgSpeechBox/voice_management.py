# -*- coding: utf-8 -*-
"""Voice selection, profile management, and per-voice slider storage — mixin for SynthDriver.

Provides:
- _get_voice / _set_voice
- _getAvailableVoices
- _reapplyVoiceProfile
- _initPerVoiceStorage (called from __init__)
"""

from __future__ import annotations

from collections import OrderedDict

from logHandler import log
from synthDriverHandler import VoiceInfo

from .constants import VOICE_PROFILE_PREFIX, voices


class VoiceManagementMixin:
    """Voice selection, profile management, and per-voice slider storage."""

    def _initPerVoiceStorage(self):
        """Initialize per-voice slider storage dicts.

        Called from SynthDriver.__init__ to set up the dicts before
        super().__init__() triggers NVDA's settings replay.
        """
        self._perVoiceTilt = {}
        self._perVoiceNoiseGlottalMod = {}
        self._perVoicePitchSyncF1 = {}
        self._perVoicePitchSyncB1 = {}
        self._perVoiceSpeedQuotient = {}
        self._perVoiceAspirationTilt = {}
        self._perVoiceCascadeBwScale = {}
        self._perVoiceVoiceTremor = {}
        self._perVoiceHeadSize = {}
        self._perVoiceFrameExCreakiness = {}
        self._perVoiceFrameExBreathiness = {}
        self._perVoiceFrameExJitter = {}
        self._perVoiceFrameExShimmer = {}
        self._perVoiceFrameExSharpness = {}
        self._perVoiceChorusDepth = {}
        self._perVoiceChorusDetune = {}

    def _reapplyVoiceProfile(self):
        """Re-apply the current voice setting to ensure the frontend profile is in sync.

        This is called after __init__ completes to handle the case where NVDA restores
        the saved voice setting but the frontend profile wasn't properly applied.
        """
        try:
            voice = getattr(self, "_curVoice", "Adam")
            if voice and voice.startswith(VOICE_PROFILE_PREFIX):
                profileName = voice[len(VOICE_PROFILE_PREFIX):]
                if hasattr(self, "_frontend") and self._frontend:
                    self._frontend.setVoiceProfile(profileName)
                    self._usingVoiceProfile = True
                    self._activeProfileName = profileName
                    # Also apply voicing tone for this profile
                    self._applyVoicingTone(profileName)
                    log.debug(f"TGSpeechBox: reapplied voice profile '{profileName}'")
            else:
                # Python preset - ensure voicing tone is at defaults
                self._applyVoicingTone("")
        except Exception:
            log.debug("TGSpeechBox: _reapplyVoiceProfile failed", exc_info=True)

    def _get_voice(self):
        return getattr(self, "_curVoice", "Adam") or "Adam"

    def _set_voice(self, voice):
        try:
            # CRITICAL: Do not force "Adam" if validation fails.
            # If NVDA asks for a profile we haven't loaded yet, accept it.
            # Forcing "Adam" here causes the settings loss on Escape.

            # Check if voice is actually changing
            oldVoice = getattr(self, "_curVoice", None)
            voiceChanged = (oldVoice is not None and oldVoice != voice)

            # Save current slider values for the OLD voice before switching
            if voiceChanged and oldVoice:
                self._perVoiceTilt[oldVoice] = getattr(self, "_curVoiceTilt", 50)
                self._perVoiceNoiseGlottalMod[oldVoice] = getattr(self, "_curNoiseGlottalMod", 0)
                self._perVoicePitchSyncF1[oldVoice] = getattr(self, "_curPitchSyncF1", 50)
                self._perVoicePitchSyncB1[oldVoice] = getattr(self, "_curPitchSyncB1", 50)
                self._perVoiceSpeedQuotient[oldVoice] = getattr(self, "_curSpeedQuotient", 50)
                self._perVoiceAspirationTilt[oldVoice] = getattr(self, "_curAspirationTilt", 50)
                self._perVoiceCascadeBwScale[oldVoice] = getattr(self, "_curCascadeBwScale", 50)
                self._perVoiceVoiceTremor[oldVoice] = getattr(self, "_curVoiceTremor", 0)
                self._perVoiceHeadSize[oldVoice] = getattr(self, "_curHeadSize", 50)
                self._perVoiceFrameExCreakiness[oldVoice] = getattr(self, "_curFrameExCreakiness", 0)
                self._perVoiceFrameExBreathiness[oldVoice] = getattr(self, "_curFrameExBreathiness", 0)
                self._perVoiceFrameExJitter[oldVoice] = getattr(self, "_curFrameExJitter", 0)
                self._perVoiceFrameExShimmer[oldVoice] = getattr(self, "_curFrameExShimmer", 0)
                self._perVoiceFrameExSharpness[oldVoice] = getattr(self, "_curFrameExSharpness", 50)
                self._perVoiceChorusDepth[oldVoice] = getattr(self, "_curChorusDepth", 0)
                self._perVoiceChorusDetune[oldVoice] = getattr(self, "_curChorusDetune", 33)

            self._curVoice = voice

            # Restore slider values for the NEW voice (or default if never set)
            if voiceChanged and voice:
                self._curVoiceTilt = self._perVoiceTilt.get(voice, 50)
                self._curNoiseGlottalMod = self._perVoiceNoiseGlottalMod.get(voice, 0)
                self._curPitchSyncF1 = self._perVoicePitchSyncF1.get(voice, 50)
                self._curPitchSyncB1 = self._perVoicePitchSyncB1.get(voice, 50)
                self._curSpeedQuotient = self._perVoiceSpeedQuotient.get(voice, 50)
                self._curAspirationTilt = self._perVoiceAspirationTilt.get(voice, 50)
                self._curCascadeBwScale = self._perVoiceCascadeBwScale.get(voice, 50)
                self._curVoiceTremor = self._perVoiceVoiceTremor.get(voice, 0)
                # Head size default: check voice preset for a per-voice default
                hsDefault = voices.get(voice, {}).get("headSize", 50)
                self._curHeadSize = self._perVoiceHeadSize.get(voice, hsDefault)
                self._curFrameExCreakiness = self._perVoiceFrameExCreakiness.get(voice, 0)
                self._curFrameExBreathiness = self._perVoiceFrameExBreathiness.get(voice, 0)
                self._curFrameExJitter = self._perVoiceFrameExJitter.get(voice, 0)
                self._curFrameExShimmer = self._perVoiceFrameExShimmer.get(voice, 0)
                self._curFrameExSharpness = self._perVoiceFrameExSharpness.get(voice, 50)
                self._curChorusDepth = self._perVoiceChorusDepth.get(voice, 0)
                self._curChorusDetune = self._perVoiceChorusDetune.get(voice, 33)
                # During init, skip DLL calls — step 10 does one final apply.
                if getattr(self, "_initComplete", False):
                    self._pushFrameExDefaultsToFrontend()

            # Handle voice profile vs Python preset
            if voice and voice.startswith(VOICE_PROFILE_PREFIX):
                profileName = voice[len(VOICE_PROFILE_PREFIX):]
                self._usingVoiceProfile = True
                self._activeProfileName = profileName

                if hasattr(self, "_frontend") and self._frontend:
                    self._frontend.setVoiceProfile(profileName)
                    # During init, skip — step 10 applies voicing tone once.
                    if getattr(self, "_initComplete", False):
                        self._applyVoicingTone(profileName)
            else:
                self._usingVoiceProfile = False
                self._activeProfileName = ""

                if hasattr(self, "_frontend") and self._frontend:
                    self._frontend.setVoiceProfile("")
                    # During init, skip — step 10 applies voicing tone once.
                    if getattr(self, "_initComplete", False):
                        self._applyVoicingTone("")

            if self.exposeExtraParams:
                for paramName in getattr(self, "_extraParamNames", []):
                    setattr(self, f"speechPlayer_{paramName}", 50)
        except Exception:
            # Never crash during settings application
            log.debug("TGSpeechBox: _set_voice failed", exc_info=True)

    def _getAvailableVoices(self):
        try:
            d = OrderedDict()
            # Python presets first
            for name in sorted(voices):
                d[name] = VoiceInfo(name, name)
            # Voice profiles from phonemes.yaml (if any)
            for profileName in sorted(getattr(self, "_voiceProfiles", []) or []):
                voiceId = f"{VOICE_PROFILE_PREFIX}{profileName}"
                # Display name includes profile name, description notes it's a profile
                d[voiceId] = VoiceInfo(voiceId, f"{profileName} (profile)")

            # CRITICAL: Ensure the current voice is always in availableVoices.
            # This prevents KeyError in speechDictHandler.loadVoiceDict() when
            # NVDA calls loadSettings() on Escape (onDiscard), which does:
            #   synth.availableVoices[synth.voice].displayName
            # If current voice isn't in the dict, that line throws KeyError,
            # which causes loadSettings() to fall back to "Adam".
            curVoice = getattr(self, "_curVoice", None)
            if curVoice and curVoice not in d:
                if curVoice.startswith(VOICE_PROFILE_PREFIX):
                    profileName = curVoice[len(VOICE_PROFILE_PREFIX):]
                    d[curVoice] = VoiceInfo(curVoice, f"{profileName} (profile)")
                else:
                    # Unknown Python preset - add it anyway
                    d[curVoice] = VoiceInfo(curVoice, curVoice)

            return d
        except Exception:
            # Return at least the default voice
            return OrderedDict([("Adam", VoiceInfo("Adam", "Adam"))])
