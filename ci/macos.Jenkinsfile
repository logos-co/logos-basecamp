#!/usr/bin/env groovy

library 'status-jenkins-lib@add-logos-app-macos-signing-credentials'

def isPRBuild = utils.isPRBuild()

pipeline {
  agent { label "macos && ${getArch()} && nix-2.24" }

  parameters {
    booleanParam(
      name: 'RELEASE',
      description: 'Decides whether release credentials are used.',
      defaultValue: params.RELEASE ?: false
    )
  }

  options {
    timestamps()
    ansiColor('xterm')
    timeout(time: 30, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
      artifactNumToKeepStr: '1',
    ))
    disableConcurrentBuilds(
      abortPrevious: isPRBuild
    )
    /* Allows combined build to copy */
    copyArtifactPermission('/logos-basecamp/*')
  }

  environment {
    PLATFORM = "macos/${getArch()}"
    ARTIFACT = "pkg/${utils.pkgFilename(name: 'LogosBasecamp', ext: 'dmg', arch: getArch())}"
    NOTARIZED_ARTIFACT = "pkg/${utils.pkgFilename(name: 'LogosBasecamp-notarized', ext: 'dmg', arch: getArch())}"
  }

  stages {
    stage('Smoke Test') {
      steps { script {
        nix.flake('smoke-test-bundle')
        sh 'cat ./result/smoke-test.log'
      } }
    }

    stage('Build MacOS App Bundle') {
      steps { script {
        nix.flake('bin-macos-app')
      } }
    }

    stage('Package') {
      when {
        expression { !utils.isReleaseBuild() }
      }
      steps {
        sh 'mkdir -p pkg'
        sh """
          TMPDIR=\$(mktemp -d)
          cp -a result/LogosBasecamp.app "\$TMPDIR/LogosBasecamp.app"
          chmod -R u+w "\$TMPDIR/LogosBasecamp.app"
          hdiutil create -volname "LogosBasecamp" \
            -srcfolder "\$TMPDIR/LogosBasecamp.app" \
            -ov -format UDZO \
            -puppetstrings \
            "${env.ARTIFACT}"
          rm -rf "\$TMPDIR"
        """
      }
    }

    stage('Sign & Notarize') {
      when {
        expression { utils.isReleaseBuild() }
      }
      steps {
        script {
          logos.signAndNotarizeApp(
            bundlePath: 'result/LogosBasecamp.app',
            outputPath: env.NOTARIZED_ARTIFACT,
            timeout: '30m'
          )
        }
      }
    }

    stage('Upload') {
      steps { script {
        def uploadFile = utils.isReleaseBuild() ? env.NOTARIZED_ARTIFACT : env.ARTIFACT
        env.PKG_URL = s5cmd.upload(uploadFile)
        jenkins.setBuildDesc(DMG: env.PKG_URL)
      } }
    }

    stage('Archive') {
      steps { script {
        def uploadFile = utils.isReleaseBuild() ? env.NOTARIZED_ARTIFACT : env.ARTIFACT
        archiveArtifacts(uploadFile)
      } }
    }
  }

  post {
    success { script { github.notifyPR(true) } }
    failure { script { github.notifyPR(false) } }
    cleanup {
      cleanWs(disableDeferredWipeout: true)
      dir(env.WORKSPACE_TMP) { deleteDir() }
    }
  }
}

def getArch() {
  def tokens = Thread.currentThread().getName().split('/')
  for (def arch in ['x86_64', 'aarch64']) {
    if (tokens.contains(arch)) { return arch }
  }
}
